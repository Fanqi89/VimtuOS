#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/preload_update_test.py - 批次 A 后半：preload64 预热 + update64 闭环 端到端验收

做什么（骨架照 tests/proc64_test.py：造"已装好系统 + 主分区已格式化"的测试盘 -> 无头启动 -> 读串口断言）：
  1) preload64：断言启动打点齐 + 数字真实可用：
       [PRELOAD64] glyphs prewarmed=<n> ms=<n>   （n >= 200；本机实测值见输出）
       [PRELOAD64] icons cached=<n>              （== 4：3 桌面图标 + 开始图标）
       [PRELOAD64] first paint before=<n> after=<n> cycles
         ★ 断言 after <= before。若真实测量出现 after > before，本脚本**不会伪造**：
           直接在输出里打出两个真实数字并以 FAIL 结束（如实报告）。
  2) sysstate64：两个新模块已注册并复核通过（preload64 / update64）。
  3) update64 闭环（标记 -> 应用 -> store -> /update.done -> 重启）：
       a) 第一遍启动（干净盘）：[UPDATE64] no pending；
       b) 宿主侧往盘上的 VimtuFS2 根目录写 /update.pending（内容 ver=0.2.0）；
       c) 第二遍启动：应看到 [UPDATE64] apply pending ver=0.2.0 at boot
          -> [UPDATE64] applied ver=0.2.0 ... flush=0 ... path=/update.done
          -> [UPDATE64] restarting to finish update -> 复位（QEMU 退出）；
       d) 宿主侧解析 raw 镜像：/update.done 存在且内容含 0.2.0，/update.pending 已消失；
       e) 第三遍启动：store64 从盘上读到 update.applied=0.2.0（[STORE64] dump update.applied=0.2.0
          + [UPDATE64] init ... applied=0.2.0），且不再触发重启（[UPDATE64] no pending）、桌面照常起来。
  4) 终端没有"尚未支持"命令：串口不得出现 [TERM] unsupported。
  5) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

用法（必须用 Windows 原生 Python）：
    py -3 tests\\preload_update_test.py
    py -3 tests\\preload_update_test.py --img <已装好的磁盘镜像> --timeout 150
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# 夹具与启动流程直接复用 proc64_test.py（同一套"已安装系统 + VimtuFS2 主分区"）
import proc64_test as p64          # noqa: E402

SECTOR = p64.SECTOR
PART_MAIN_LBA = p64.PART_MAIN_LBA

MUST = [
    ("[PRELOAD64] init cache_cap=",        "preload64 初始化（缓存容量 + LRU + 预热集字数）"),
    ("[PRELOAD64] prewarm set fits lru=",  "预热集能全部驻留 LRU（幂等前提，如实自检）"),
    ("[PRELOAD64] glyphs prewarmed=",      "字形预光栅化数量 + 实测耗时"),
    ("[PRELOAD64] icons cached=",          "图标预缩放数量（3 桌面图标 + 开始图标 = 4）"),
    ("[PRELOAD64] first paint before=",    "首帧代表工作：预热前后 rdtsc64 实测"),
    ("[SYS64] module preload64 registered", "preload64 注册进 sysstate64 模块表"),
    ("[SYS64] module preload64 init ok",   "preload64 通过 STARTING 复核"),
    ("[UPDATE64] init version=",           "update64 初始化（版本 / store 里的 applied / 载体）"),
    ("[UPDATE64] no pending",              "干净盘：没有 update 标记文件"),
    ("[SYS64] module update64 registered", "update64 注册进 sysstate64 模块表"),
    ("[SYS64] module update64 init ok",    "update64 通过 STARTING 复核"),
    ("[TASK64] api selftest PASS",          "task64 新增 API（find_by_name/mark_critical/set_slice/force_remove/diag）启动自检"),
    ("[TASK64] diag slot=",                 "任务表诊断逐槽打点（task64_diag64）"),
    ("[GUI64] ready",                      "预热/更新检查后桌面照常起来"),
]

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[TERM] unsupported",
    "[UPDATE64] pending invalid",
    "[PRELOAD64] WARN prewarm set exceeds LRU",
]


# ---------------------------------------------------------------------------
# VimtuFS2（宿主侧只读/写根目录单层文件；布局与 kernel/vfs64.cpp 的 VFS_I_* 一致）
# ---------------------------------------------------------------------------
def _sb(buf, base):
    total = struct.unpack_from("<I", buf, base + 20)[0]
    return {
        "total": total,
        "bmn": struct.unpack_from("<I", buf, base + 32)[0],
        "ino_start": struct.unpack_from("<I", buf, base + 36)[0],
        "inodes": struct.unpack_from("<I", buf, base + 40)[0],
        "data_start": struct.unpack_from("<I", buf, base + 48)[0],
    }
def _inode_off(base, sb, idx):
    blk = idx // 8
    return base + (sb["ino_start"] + blk) * SECTOR + (idx % 8) * 64


def vfs_find(buf, base, name):
    sb = _sb(buf, base)
    want = name.encode()
    for i in range(1, sb["inodes"]):
        off = _inode_off(base, sb, i)
        if buf[off] != 1:                      # TYPE_FILE
            continue
        if buf[off + 1] != len(want):
            continue
        if bytes(buf[off + 32:off + 32 + len(want)]) == want:
            return sb, off
    return sb, None


def vfs_read(buf, base, name):
    """返回文件内容 bytes；不存在返回 None。"""
    sb, off = vfs_find(buf, base, name)
    if off is None:
        return None
    size = struct.unpack_from("<I", buf, off + 4)[0]
    out = bytearray()
    for d in range(4):
        if len(out) >= size:
            break
        blk = struct.unpack_from("<I", buf, off + 8 + 4 * d)[0]
        if blk == 0:
            continue
        start = base + blk * SECTOR
        out += buf[start:start + min(SECTOR, size - len(out))]
    if len(out) < size:                        # 一级间接块
        ind = struct.unpack_from("<I", buf, off + 24)[0]
        if ind:
            ioff = base + ind * SECTOR
            while len(out) < size:
                p = (len(out) // SECTOR) - 4
                if p < 0 or p >= 128:
                    break
                blk = struct.unpack_from("<I", buf, ioff + 4 * p)[0]
                if blk == 0:
                    break
                start = base + blk * SECTOR
                out += buf[start:start + min(SECTOR, size - len(out))]
    return bytes(out[:size])


def _bm_used(buf, base, sb, blk):
    m = blk // 4096
    off = base + (1 + m) * SECTOR + (blk % 4096) // 8
    return (buf[off] >> ((blk % 4096) & 7)) & 1


def _bm_mark(buf, base, sb, blk):
    m = blk // 4096
    off = base + (1 + m) * SECTOR + (blk % 4096) // 8
    buf[off] |= 1 << ((blk % 4096) & 7)


def vfs_write_small(buf, base, name, data):
    """在根目录写一个"小的"文件（<= 2048B，只用 4 个直接块）——宿主侧造 /update.pending 用。"""
    assert len(data) <= 2048
    sb = _sb(buf, base)
    if vfs_find(buf, base, name)[1] is not None:
        raise RuntimeError("fixture already has " + name)
    # 1) 找一个空 inode
    ino = -1
    for i in range(1, sb["inodes"]):
        if buf[_inode_off(base, sb, i)] == 0:
            ino = i
            break
    if ino < 0:
        raise RuntimeError("no free inode")
    # 2) 分配数据块（bit=1 表示已用）
    blocks = []
    blk = sb["data_start"]
    need = (len(data) + SECTOR - 1) // SECTOR
    while len(blocks) < need and blk < sb["total"]:
        if not _bm_used(buf, base, sb, blk):
            blocks.append(blk)
            _bm_mark(buf, base, sb, blk)
        blk += 1
    if len(blocks) < need:
        raise RuntimeError("no free block")
    # 3) 写数据
    for i, b in enumerate(blocks):
        start = base + b * SECTOR
        chunk = data[i * SECTOR:(i + 1) * SECTOR]
        buf[start:start + len(chunk)] = chunk
        if len(chunk) < SECTOR:
            buf[start + len(chunk):start + SECTOR] = b"\0" * (SECTOR - len(chunk))
    # 4) 写 inode（布局与 vfs64.cpp 的 VFS_I_* 一致）+ CRC32
    off = _inode_off(base, sb, ino)
    ino_bytes = bytearray(64)
    ino_bytes[0] = 1
    ino_bytes[1] = len(name)
    struct.pack_into("<I", ino_bytes, 4, len(data))
    for i, b in enumerate(blocks[:4]):
        struct.pack_into("<I", ino_bytes, 8 + 4 * i, b)
    struct.pack_into("<I", ino_bytes, 28, 0)         # parent = root
    ino_bytes[32:32 + len(name)] = name.encode()
    struct.pack_into("<I", ino_bytes, 60, zlib.crc32(bytes(ino_bytes[:60])) & 0xFFFFFFFF)
    buf[off:off + 64] = ino_bytes


def boot(qemu, img, tag, logdir, timeout, ready=True):
    """无头启动；ready=True 等到 [GUI64] ready，ready=False 等到进程退出（自动重启）或超时。"""
    serial = os.path.join(logdir, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    args = [qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % p64.q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % p64.q(serial),
            "-no-reboot"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    exited = False

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = slog()
            if ready and "[GUI64] ready" in s:
                time.sleep(2.0)
                break
            if proc.poll() is not None:
                exited = True
                break
            if not ready and "restarting to finish update" in s:
                time.sleep(1.0)
                if proc.poll() is not None:
                    exited = True
                    break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return slog(), exited


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None, help="已装好的系统盘镜像；缺省用 build64/system.img 自动造夹具")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    if args.img:
        img = args.img
        print("[plup] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(p64.SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % p64.SYSTEM_IMG)
            return 2
        img = p64.prepare_fixture()
        if not img:
            sys.stderr.write("造测试盘失败（%s 不合法）\n" % p64.SYSTEM_IMG)
            return 2
        print("[plup] 测试盘已生成：%s（主分区 LBA %d 已格式化）" % (img, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_plup_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 preload/update acceptance（BIOS/SeaBIOS）===")
    log1, early1 = boot(qemu, img, "boot1", tmp, args.timeout)
    if early1:
        print("  [!] 第一遍启动提前退出（不应发生：干净盘没有 pending）")

    print("--- 必须出现（preload64 / 模块注册 / 干净盘）---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log1)

    print("--- preload64：真实测量数字 ---")
    m = re.search(r"\[PRELOAD64\] glyphs prewarmed=(\d+) ms=(\d+)", log1)
    check("解析到 glyphs prewarmed=<n> ms=<n>", bool(m), m.group(0) if m else "")
    if m:
        check("预热字形数 >= 200（top200 汉字 + ASCII 三 face）", int(m.group(1)) >= 200,
              "glyphs=%s ms=%s" % (m.group(1), m.group(2)))
    mi = re.search(r"\[PRELOAD64\] icons cached=(\d+)", log1)
    check("icons cached == 4（3 桌面图标 + 开始图标）", bool(mi) and int(mi.group(1)) == 4,
          ("icons=%s" % mi.group(1)) if mi else "")
    mp = re.search(r"\[PRELOAD64\] first paint before=(\d+) after=(\d+) cycles", log1)
    check("解析到 first paint before/after cycles", bool(mp), mp.group(0) if mp else "")
    if mp:
        before, after = int(mp.group(1)), int(mp.group(2))
        check("预热后首帧代表工作 <= 预热前（真实测量 after=%d before=%d）" % (after, before),
              after <= before,
              "after=%d before=%d delta=%d" % (after, before, before - after))
    else:
        check("预热后首帧代表工作 <= 预热前", False)

    print("--- update64：宿主侧写标记 -> 启动应用 -> store/done/重启 -> 再启动 ---")
    # b) 宿主侧往盘上写 /update.pending（内容 ver=0.2.0）
    with open(img, "rb") as f:
        buf = bytearray(f.read())
    vfs_write_small(buf, PART_MAIN_LBA * SECTOR, "update.pending", b"ver=0.2.0\n")
    with open(img, "wb") as f:
        f.write(buf)
    check("宿主侧已写入 /update.pending（ver=0.2.0）", vfs_read(buf, PART_MAIN_LBA * SECTOR, "update.pending") == b"ver=0.2.0\n")

    # c) 第二遍启动：应用 + 自动重启（不是等 [GUI64] ready，而是等退出）
    log2, exited2 = boot(qemu, img, "boot2", tmp, args.timeout, ready=False)
    check("[UPDATE64] apply pending ver=0.2.0 at boot", "[UPDATE64] apply pending ver=0.2.0 at boot" in log2)
    check("[UPDATE64] applied ver=0.2.0 ...（store + /update.done 打点）",
          "[UPDATE64] applied ver=0.2.0 from=0.1.0" in log2)
    check("store flush 成功（flush=0）", "flush=0" in log2 and "[UPDATE64] applied ver=0.2.0" in log2)
    check("[UPDATE64] restarting to finish update", "[UPDATE64] restarting to finish update" in log2)
    check("应用后走软重启（[SYS64] soft restart requested）", "[SYS64] soft restart requested" in log2)
    check("第二遍启动以复位结束（QEMU -no-reboot 提前退出）", exited2)

    # d) 宿主侧解析 raw 镜像：/update.done 在、/update.pending 没了
    with open(img, "rb") as f:
        buf = bytearray(f.read())
    base = PART_MAIN_LBA * SECTOR
    done = vfs_read(buf, base, "update.done")
    check("raw 镜像里 /update.done 存在", done is not None, (repr(done) if done else ""))
    check("/update.done 内容含 applied ver=0.2.0", bool(done) and b"applied ver=0.2.0" in done,
          (done.decode("ascii", "replace") if done else ""))
    check("raw 镜像里 /update.pending 已被删除（消失）", vfs_read(buf, base, "update.pending") is None)

    # e) 第三遍启动：store 里读到 applied、不再触发重启、桌面起来
    log3, early3 = boot(qemu, img, "boot3", tmp, args.timeout)
    check("[UPDATE64] init ... applied=0.2.0（store64 从盘上读回）",
          bool(re.search(r"\[UPDATE64\] init version=0\.1\.0 applied=0\.2\.0", log3)),
          next((ln for ln in log3.splitlines() if "[UPDATE64] init" in ln), ""))
    check("[STORE64] dump update.applied=0.2.0", "[STORE64] dump update.applied=0.2.0" in log3)
    check("第三遍不再触发更新（[UPDATE64] no pending）", "[UPDATE64] no pending" in log3)
    check("第三遍桌面照常起来（[GUI64] ready）", "[GUI64] ready" in log3)
    if early3:
        print("  [!] 第三遍启动提前退出（不应发生）")

    print("--- 禁止出现 ---")
    for needle in FORBIDDEN:
        check("boot1/boot2/boot3 不得出现 %s" % needle,
              (needle not in log1) and (needle not in log2) and (needle not in log3))

    if args.keep:
        print("[plup] 串口日志目录：%s" % tmp)

    print("--- boot1 serial tail ---")
    for line in [x for x in log1.splitlines() if x.strip()][-12:]:
        print("   | " + line[:170])

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
