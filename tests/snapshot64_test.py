"""tests/snapshot64_test.py - VimtuFS2 v5 **只读快照**（全量克隆，非 COW）验收

判据（三层，互相独立）：
  A) 代码结构（不需要 QEMU）：快照表格式（SNP1 + 8 项 {id,start,blocks,taken,state,slot}）/ take / mount / list + 只读写保护
  B) 串口真跑：`[VFS64] snapshot ok id=1 slot=.. blocks=128 ro_enforced=1 timepoint_isolated=1 tree_copied=1 cow=0 step=0`
     —— `step` 是自检里**第一条**失败判据的编号（0 = 全过），排障不用猜；
  C) 总自检与打点：`[SNAP] take ok ... files=.. dirs=0 links_skipped=0 bytes=.. cow=0 ro=1`（真的拷了整棵树）与 `[VFS64] write denied reason=volume-read-only(snapshot)`（只读不是纸面约定）

边界（如实）：快照 = **全量拷贝**（非 COW）：空间 = 快照区大小，时间 = 全量读 + 全量写；符号链接不进快照

用法：py -3 tests\\snapshot64_test.py [--qemu 路径] [--keep]
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
    check("快照表格式（magic SNP1 / 8 项 / 每项 id+start+blocks+taken+state+slot）",
          "SN_MAGIC" in src and "SN_MAX     = 8u" in src and "SN_ENT     = 24u" in src, "")
    check("拍快照 = 预留连续区 + 在预留区格式化独立 v5 卷 + 递归拷贝整棵树",
          "alloc_run64(n)" in src and "snap_copy_dir64" in src and "vfs64_format(drive, abs_start, n)" in src, "")
    check("快照卷只读：写盘出口 dev_write_at 上拦只读卷",
          "volume-read-only(snapshot)" in src, "")
    check("快照挂载走卷槽（vfs64_snapshot_mount64 -> mount_slot64 + ro=1）",
          "vfs64_snapshot_mount64" in src and "g_vol[cs].ro = true" in src, "")
    check("接口在头文件（take/list/mount + Vfs64SnapInfo64）",
          all(k in hdr for k in ("vfs64_snapshot_take64", "vfs64_snapshot_list64",
                                 "vfs64_snapshot_mount64", "Vfs64SnapInfo64")), "")
    print("=== B) 真 QEMU ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\\n")
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败\\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_snapshot_")
    disk = os.path.join(tmp, "snapshot.img")
    shutil.copyfile(base, disk)                  # ★ 副本：绝不直接引导共享夹具
    serial = os.path.join(tmp, "serial.log")
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-snapshot", "-drive", "format=raw,file=%s" % q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_re(serial, r"\[VFS64\] snapshot ok id=(\d+) slot=(\d+) blocks=(\d+) ro_enforced=1 "
                             r"timepoint_isolated=1 tree_copied=1 cow=0 step=(\d+)", 300, proc)
        check("[VFS64] snapshot ok 行出现（自检 bit21 跑完）", m is not None,
              m.group(0) if m else "（没等到 snapshot ok 行）")
        if m:
            check("快照 id = 1（表里第一个）", m.group(1) == "1", "id=%s" % m.group(1))
            check("快照卷块数 = 请求的 128 块", m.group(3) == "128", "blocks=%s" % m.group(3))
            check("bit21 每一条判据都过（step=0）", m.group(4) == "0", "step=%s" % m.group(4))
        mt = wait_re(serial, r"\[SNAP\] take ok id=1 start_blk=\d+ blocks=128 slot=\d+ files=(\d+) "
                             r"dirs=(\d+) links_skipped=(\d+) bytes=(\d+) cow=0 ro=1", 300, proc)
        check("[SNAP] take ok 行出现（真的拷了整棵树）", mt is not None,
              mt.group(0) if mt else "（没等到 [SNAP] take ok 行）")
        if mt:
            check("至少拷了 1 个文件（files >= 1）", int(mt.group(1)) >= 1, "files=%s" % mt.group(1))
            check("拷了非 0 字节（bytes >= 1）", int(mt.group(4)) >= 1, "bytes=%s" % mt.group(4))
        check("只读卷真的拒写（[VFS64] write denied reason=volume-read-only(snapshot)）",
              "reason=volume-read-only(snapshot)" in slog(serial), "")
        check("特性层打点：[VFS64] feature on bit=8 name=snapshot",
              "[VFS64] feature on bit=8 name=snapshot" in slog(serial), "")
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
