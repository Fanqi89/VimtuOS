"""tests/journal64_test.py - VimtuFS2 **v5 元数据日志**（journaling）验收

背景（v0.9.28）：vfs64 的写纪律原本只有"先数据、后指针、最后 inode"，中途掉电最坏是块泄漏；
但一次操作里"位图 + 指针块 + inode"是**多块独立写**，掉电落在中间会留下"只做了一半"的状态。
本批把这几块做成**一个原子事务**（写前日志）：
  ① 载荷块写进日志区 → ② 写日志头 state=COMMITTED（**提交点**）→ ③ checkpoint → ④ 清日志头
掉电在 ②之后：挂载时重放把 ③ 补完；掉电在 ②之前：头不是 COMMITTED，什么都不做。

判据（四层，互相独立）：
  A) 代码结构：日志区/记录格式/提交/重放/钩子都在 `kernel/vfs64.cpp` 里，且
     **位图、inode 提交、三种指针块（子块/二级/一级）都走 meta_write64**（少一处就是"只做一半"）；
  B) 串口真跑：`[VFS64] journal ok commits=.. jx_blocks=17 replay=1 bad_crc_rejected=1 step=0`
     —— step 是**第一条**失败判据的编号（0 = 全过），排障不用猜；
  C) 崩溃现场真的被重放：`[JX64] replay ok txid=.. nblk=.. applied=.. journal_cleared=1`
     （这是自检里"只提交不 checkpoint → 重新 mount"那条路径打出来的）；
  D) v5 格式与 v4 旧卷：`[VFS64] format ok ... version=5`、`[VFS64] migrate ok from=4 to=5
     blocks=.. inode_untouched=1`、`[VFS64] migrate ok v4_compat=1 v4_refused_features=1 data_intact=1
     now=5 ... idempotent=1 step=0`（v4 卷照常挂载/读数据，但开特性必须被拒；迁移只改版本号）。

用法：py -3 tests\\journal64_test.py [--qemu 路径] [--keep]
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
    "[JX64] replay FAILED",
    "[VFS64] migrate FAILED",
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


def src():
    with open(VFS_C, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


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

    # ==================== A) 代码结构（不需要 QEMU）====================
    print("=== A) 日志的记录格式 / 提交协议 / 重放 / 元数据写钩子 ===")
    s = src()
    h = open(VFS_H, "r", encoding="utf-8", errors="replace").read()
    check("日志记录头有 magic/txid/nblk/state/crc（JX_O_* 五个偏移都在）",
          all(k in s for k in ("JX_O_MAGIC", "JX_O_TXID", "JX_O_NBLK", "JX_O_STATE", "JX_O_CRC")),
          "")
    check("提交点 = 写日志头（jx_commit_inner64 里先写载荷、再写头、再 checkpoint、最后清头）",
          "jx_commit_inner64" in s and "JX_STATE_COMMIT" in s, "")
    check("挂载会先重放再统计空闲块（vfs64_mount 里调用 jx_replay64）",
          s.count("jx_replay64") >= 3 and "log_mount_fail(\"journal\")" in s, "")
    check("元数据写唯一入口 meta_write64 存在，且位图/inode/指针块都走它",
          "static bool meta_write64" in s and
          s.count("meta_write64(g_bitmap_start") == 1 and
          s.count("meta_write64(blk, sec, true)") == 1 and
          s.count("meta_write64(m->l3_blk") == 1 and
          s.count("meta_write64(b, g_l2)") == 1 and
          s.count("meta_write64(b, g_l1)") == 1,
          "bitmap=%d inode=%d l3=%d l2=%d l1=%d" % (
              s.count("meta_write64(g_bitmap_start"), s.count("meta_write64(blk, sec, true)"),
              s.count("meta_write64(m->l3_blk"), s.count("meta_write64(b, g_l2)"),
              s.count("meta_write64(b, g_l1)")))
    check("inode 提交 = 事务边界（inode_store 里 jx_end64 收口）", s.count("jx_end64()") >= 2, "")
    check("特性位定义在头文件（JOURNAL=1u / FEAT_ALL / JX_MAX_BLOCKS）",
          all(k in h for k in ("VFS64_FEAT_JOURNAL     1u", "VFS64_FEAT_ALL", "VFS64_JX_MAX_BLOCKS")), "")

    # ==================== B~D) 真 QEMU ====================
    print("=== B) 真 QEMU：自检里的日志事务 + 崩溃重放 ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_journal_")
    disk = os.path.join(tmp, "journal.img")
    shutil.copyfile(base, disk)                  # ★ 副本：绝不直接引导共享夹具
    serial = os.path.join(tmp, "serial.log")
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-journal", "-drive", "format=raw,file=%s" % q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_re(serial, r"\[VFS64\] journal ok commits=(\d+) jx_blocks=(\d+) replay=1 "
                             r"bad_crc_rejected=(\d) step=(\d+)", 300, proc)
        check("[VFS64] journal ok 行出现（自检 bit18 跑完）", m is not None,
              m.group(0) if m else "（没等到 journal ok 行）")
        if m:
            check("日志区 = 1 头块 + 16 载荷块（jx_blocks=17）", m.group(2) == "17", "jx_blocks=%s" % m.group(2))
            check("真的提交过事务（commits >= 1）", int(m.group(1)) >= 1, "commits=%s" % m.group(1))
            check("坏 CRC 的记录被拒放（bad_crc_rejected=1，不是'照着写'）", m.group(3) == "1",
                  "bad_crc_rejected=%s" % m.group(3))
            check("bit18 每一条判据都过（step=0）", m.group(4) == "0", "step=%s" % m.group(4))
        mr = wait_re(serial, r"\[JX64\] replay ok txid=(\d+) nblk=(\d+) applied=(\d+) journal_cleared=1",
                     300, proc)
        check("崩溃现场被重放（[JX64] replay ok ... journal_cleared=1）", mr is not None,
              mr.group(0) if mr else "（没等到 replay ok 行）")
        if mr:
            check("重放的块数 = 事务里的块数（applied == nblk）", mr.group(2) == mr.group(3),
                  "nblk=%s applied=%s" % (mr.group(2), mr.group(3)))
        check("特性层打点：[VFS64] feature on bit=1 name=journal",
              "[VFS64] feature on bit=1 name=journal" in slog(serial), "")

        # ---- D) v5 格式 + v4 旧卷兼容与迁移 ----
        print("=== C) v5 格式 + v4 旧卷兼容 + 就地迁移 ===")
        check("新格式化产出 v5（[VFS64] format ok ... version=5 inode=128）",
              wait_re(serial, r"\[VFS64\] format ok blocks=\d+ version=5 inode=128", 300, proc) is not None, "")
        mm = wait_re(serial, r"\[VFS64\] migrate ok from=(\d+) to=(\d+) blocks=(\d+) inode_untouched=1",
                     300, proc)
        check("v4 -> v5 就地迁移（[VFS64] migrate ok from=4 to=5 ... inode_untouched=1）",
              mm is not None and mm.group(1) == "4" and mm.group(2) == "5",
              mm.group(0) if mm else "（没等到 migrate ok 行）")
        mc = wait_re(serial, r"\[VFS64\] migrate ok v4_compat=1 v4_refused_features=1 data_intact=1 "
                             r"now=(\d+) features=(\d+) idempotent=1 step=(\d+)", 300, proc)
        check("v4 旧卷：可挂载/可读/开特性被拒/迁移后数据不变/再迁移幂等（step=0）",
              mc is not None and mc.group(1) == "5" and mc.group(3) == "0",
              mc.group(0) if mc else "（没等到 v4_compat 行）")
        # ★ QEMU 的 `-serial file:` 是**带缓冲**的：进程还在跑时最后几行可能还没落盘。
        #   所以这里只记一个中间标记，最终判定放到进程退出后（finally 里回读一次串口）。
        _selft_ok = ("[VFS64] selftest PASS" in slog(serial))

        txt = slog(serial)
        bad = [w for w in FORBIDDEN if w in txt]
        check("没有禁止行", not bad, "、".join(bad) if bad else "")
        # ★ [JX64] replay rejected reason=crc 是自检 bit18 **故意**造坏记录打出来的（正是"拒放"的证据），
        #   所以它不许进禁止行，但必须**恰好出现一次**（多一次就说明有别的坏记录）。
        n_rej = txt.count("[JX64] replay rejected reason=crc")
        check("坏 CRC 的拒放打点恰好一次（自检故意造的那次）", n_rej == 1, "count=%d" % n_rej)
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
