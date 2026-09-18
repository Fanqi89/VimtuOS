#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/elf64_test.py - ELF64 加载器 + syscall 指令路径 端到端验收

做什么（与 tests/app64_test.py 同一套骨架）：
  1) 造一块"已装好系统、主分区已格式化"的 16MB 测试盘（= build64/system.img 的字节
     + 标准 MBR 分区表 + 在 LBA 8009 上用 Python 复刻 kernel/vfs64.cpp 的空 VimtuFS2 卷）；
  2) 无头启动它两遍，读串口日志断言：
     第一遍：MSR 初始化（EFER.SCE/STAR/LSTAR/FMASK）-> [ELF64] 自检 ->
             幂等把内嵌 hello.elf 装成 /hello.elf -> 从盘上 vfs64_read 读出来 ->
             校验 ELF64 头/程序头 -> 段映射 -> 建初始栈 -> ring3 ->
             程序用 **syscall 指令** write/getpid/openat/read/clock_gettime/exit ->
             回 ring0 -> 回收页（[ELF64] launch ok rc=0）-> 桌面照常起来
     第二遍：install skipped (exists)（幂等 + 证明文件真的持久化在盘上），仍然能加载运行
  3) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / [ELF64] reject / [ELF64] launch FAILED 等
     （自检里的坏样本用专门前缀 [ELF64] selftest reject，与真实拒绝区分开）

断言（与 kernel/elf64.cpp、kernel/syscall64.cpp、kernel/syscall_entry64.asm 的打点严格对应）：
  [SYSCALL] msr init EFER.SCE=1 ...      syscall 指令路径的 MSR 已配好
  [SYSCALL] selftest PASS                含 MSR 现场的自检
  [ELF64] selftest PASS                  合法 ELF 装载 + 坏样本全部被拒
  [ELF64] install ok path=/hello.elf     首次安装（第二遍为 install skipped (exists)）
  [ELF64] load path=/hello.elf entry=... 真的走 vfs64_read 从盘上读 + 解析 + 装载
  [ELF64] enter ring3 entry=...          ELF 入口进 ring3
  hello from ELF64 (syscall insn)        用户程序 write(1,...) 的原文
  [SYSCALL] insn nr=1 / nr=39 / nr=257 / nr=60   syscall 指令路径的号段命中
  [ELF64] back to kernel (ring0) rc=0    exit 后回到 ring0
  [ELF64] launch ok rc=0                 完整成功
  [APP64] launch ok ... rc=0             VAP64（int 0x80 那条路径）没被弄坏
  [GUI64] ready                          之后桌面照常起来

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\elf64_test.py
    py -3 tests\\elf64_test.py --img <已装好的磁盘镜像> --timeout 150

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "elf64_test.img")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS     # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                              # 16MB

# 第一遍启动必须出现（顺序即输出顺序）
MUST = [
    ("[SYSCALL] msr init EFER.SCE=1",                     "syscall 指令路径的 MSR 已配好（EFER.SCE/STAR/LSTAR/FMASK）"),
    ("[SYSCALL] selftest PASS",                           "系统调用侧自检（含 MSR 现场）"),
    ("[ELF64] selftest PASS",                             "ELF64 自检（合法装载 + 坏样本全被拒）"),
    ("[ELF64] selftest reject sample=",                   "自检确实跑了坏样本（专用前缀）"),
    ("[ELF64] install ok path=/hello.elf",                "把内嵌 hello.elf 装进 VimtuFS2"),
    ("[ELF64] load path=/hello.elf entry=",               "启动器真的走 vfs64_read 从盘上读 + 解析 + 装载"),
    ("[ELF64] enter ring3 entry=",                        "按 ELF 入口进 ring3"),
    ("[USER64] enter ring3 entry=",                       "底层 ring3 进入点（iretq）"),
    ("hello from ELF64 (syscall insn)",                   "ELF64 程序在 ring3 的输出原文"),
    ("[SYSCALL] insn nr=1",                               "syscall 指令：write(1)"),
    ("[SYSCALL] insn nr=39",                              "syscall 指令：getpid()"),
    ("[SYSCALL] insn nr=257",                             "syscall 指令：openat()（走真实 VFS）"),
    ("[SYSCALL] insn nr=60",                              "syscall 指令：exit()"),
    ("[ELF64] back to kernel (ring0)",                    "exit 后回到 ring0"),
    ("[USER64] back to kernel (ring0)",                   "底层 ring0 收尾"),
    ("[ELF64] launch ok rc=0",                            "ELF64 启动器完整成功（rc=0）"),
    ("[APP64] launch ok path=/hello.vap name=hello rc=0", "VAP64（int 0x80）那条老路径没被弄坏"),
    ("[USER64] selftest PASS",                            "用户态侧自检（旧路径）"),
    ("[SYSCALL] msr init EFER.SCE=1 star=",               "syscall 指令路径的 MSR 已配好（EFER.SCE/STAR 回读）"),
    (" lstar=",                                           "LSTAR 回读留证（指向 syscall_entry64.asm）"),
    (" fmask=",                                           "FMASK 回读留证（入口自动清 IF/DF/TF）"),
    ("[GUI64] ready",                                     "跑完 ELF 后桌面照常起来"),
]

# 第二遍启动必须出现（幂等 + 持久化证据）
MUST2 = [
    ("[SYSCALL] selftest PASS",                           "MSR 重新配好（每次启动都配）"),
    ("[ELF64] selftest PASS",                             "自检照旧全过"),
    ("[ELF64] install skipped (exists) /hello.elf size=", "幂等：文件已经在盘上"),
    ("[ELF64] load path=/hello.elf entry=",               "仍然从盘上读"),
    ("hello from ELF64 (syscall insn)",                   "第二次仍然跑起来并输出"),
    ("[ELF64] launch ok rc=0",                            "第二次仍然跑成功"),
    ("[GUI64] ready",                                     "桌面照常起来"),
]

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    # ★ [ELF64] reject 是**真实装载**的拒绝（自检里的坏样本用 [ELF64] selftest reject，
    #   前缀不同所以不会命中这一条）。
    "[ELF64] reject",
    "[ELF64] launch FAILED",
    "[ELF64] load FAILED",
    "[ELF64] install FAILED",
    "[APP64] launch FAILED",
    "[USER64] run FAILED",
    "[USER64] enter FAILED",
    "[SYSCALL] deny",
    # 注意：**不要**禁 "[VFS64] read: not found" —— vfs64_selftest64 的负例每次启动都会故意打它。
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 测试夹具：一块"已装好系统 + 主分区已格式化"的盘（与 app64_test.py 同一套，逐字节相同）
# ---------------------------------------------------------------------------
def _vimtufs2_format(buf, start_lba, total_sectors):
    bmn = (total_sectors + 4095) // 4096
    inodes = max(16, min(256, total_sectors // 64))
    ino_blocks = (inodes + 7) // 8
    bm_start = 1
    ino_start = bm_start + bmn
    data_start = ino_start + ino_blocks
    data_blocks = total_sectors - data_start
    if data_blocks <= 0:
        raise RuntimeError("fixture volume too small")

    sb = bytearray(SECTOR)
    sb[0:8] = b"VIMTUFS2"
    struct.pack_into("<I", sb, 8, 2)                 # version
    struct.pack_into("<I", sb, 12, SECTOR)           # sector size
    struct.pack_into("<I", sb, 16, SECTOR)           # block size
    struct.pack_into("<I", sb, 20, total_sectors)    # total blocks
    struct.pack_into("<I", sb, 24, 0)                # root inode
    struct.pack_into("<I", sb, 28, bm_start)         # bitmap start
    struct.pack_into("<I", sb, 32, bmn)              # bitmap blocks
    struct.pack_into("<I", sb, 36, ino_start)        # inode start
    struct.pack_into("<I", sb, 40, inodes)           # inode count
    struct.pack_into("<I", sb, 44, 64)               # inode bytes
    struct.pack_into("<I", sb, 48, data_start)       # data start
    struct.pack_into("<I", sb, 52, data_blocks)      # data blocks
    struct.pack_into("<I", sb, 56, 0)                # flags
    struct.pack_into("<I", sb, 60, _crc32(sb[:60]))  # superblock crc
    sb[510], sb[511] = 0x55, 0xAA

    base = start_lba * SECTOR
    buf[base:base + SECTOR] = sb

    for m in range(bmn):
        bm = bytearray(SECTOR)
        for k in range(4096):
            blk = m * 4096 + k
            if blk >= total_sectors or blk < data_start:
                bm[k >> 3] |= 1 << (k & 7)           # 元数据区/卷外：已用
        off = base + (bm_start + m) * SECTOR
        buf[off:off + SECTOR] = bm

    for i in range(ino_blocks):
        off = base + (ino_start + i) * SECTOR
        buf[off:off + SECTOR] = b"\0" * SECTOR

    root = bytearray(64)
    root[0] = 2                                      # VFS64_TYPE_DIR
    struct.pack_into("<I", root, 28, 0)              # parent = 自己
    struct.pack_into("<I", root, 60, _crc32(root[:60]))
    off = base + ino_start * SECTOR
    buf[off:off + 64] = root


def _mbr_entry(bootable, ptype, start, sectors):
    e = bytearray(16)
    e[0] = 0x80 if bootable else 0x00
    e[1:4] = b"\xFE\xFF\xFF"
    e[4] = ptype
    e[5:8] = b"\xFE\xFF\xFF"
    struct.pack_into("<I", e, 8, start)
    struct.pack_into("<I", e, 12, sectors)
    return e


def prepare_fixture():
    if not os.path.exists(SYSTEM_IMG):
        return None
    with open(SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    if len(sys_bytes) == 0 or len(sys_bytes) % SECTOR:
        return None
    if len(sys_bytes) > TARGET_SECTORS * SECTOR:
        return None
    buf = bytearray(TARGET_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes

    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA

    _vimtufs2_format(buf, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)

    with open(FIXTURE_IMG, "wb") as f:
        f.write(buf)
    return FIXTURE_IMG


def boot(qemu, img, tag, logdir, timeout):
    """无头启动一块盘，轮询串口日志到 [GUI64] ready（或超时/提前退出），返回 (log, early)。"""
    serial = os.path.join(logdir, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    args = [
        qemu, "-name", "Vimtu64-" + tag,
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    early = False

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if "[GUI64] ready" in slog():
                break
            if proc.poll() is not None:
                early = True
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return slog(), early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None,
                    help="已装好的系统盘镜像；缺省则用 build64/system.img 自动造夹具 "
                         "build64/elf64_test.img")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150, help="每遍启动的最长等待秒数")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    if args.img:
        if not os.path.exists(args.img):
            sys.stderr.write("镜像不存在：%s\n" % args.img)
            return 2
        img = args.img
        strict_fixture = False
        print("[elf64] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
            return 2
        img = prepare_fixture()
        if not img:
            sys.stderr.write("造测试盘失败（%s 不合法）\n" % SYSTEM_IMG)
            return 2
        strict_fixture = True
        print("[elf64] 测试盘已生成：%s（%d 扇区，主分区 LBA %d 已格式化）"
              % (img, TARGET_SECTORS, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_elf64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 elf64 acceptance ===")
    print("[elf64] 第一遍启动：装进去 -> 从盘上读出来 -> ELF64 装载 -> ring3 + syscall 指令")
    log1, early1 = boot(qemu, img, "boot1", tmp, args.timeout)

    print("--- 第一遍：必须出现 ---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log1)

    install1 = ("[ELF64] install ok path=/hello.elf" in log1) or \
               ("[ELF64] install skipped (exists) /hello.elf" in log1)
    check("装进去：install ok 或 install skipped (exists)", install1,
          "（本测试盘是新格式化的 -> 必须 install ok）" if strict_fixture else "")
    if strict_fixture:
        check("首次启动 = 全新安装（夹具刚格式化）",
              "[ELF64] install ok path=/hello.elf" in log1)

    print("--- 第一遍：禁止出现 ---")
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log1)
    if early1:
        print("  [!] 第一遍 QEMU 提前退出（复位/三重故障？）")

    print("[elf64] 第二遍启动：幂等 install skipped + 持久化后仍能加载运行")
    log2, early2 = boot(qemu, img, "boot2", tmp, args.timeout)

    print("--- 第二遍：必须出现 ---")
    for needle, what in MUST2:
        check("第二遍 %s（%s）" % (needle, what), needle in log2)
    if strict_fixture:
        check("第二遍确实是 skipped（文件持久化在盘上）",
              "[ELF64] install skipped (exists) /hello.elf size=" in log2)

    print("--- 第二遍：禁止出现 ---")
    for needle in FORBIDDEN:
        check("第二遍不得出现 %s" % needle, needle not in log2)
    if early2:
        print("  [!] 第二遍 QEMU 提前退出（复位/三重故障？）")

    if args.keep:
        print("[elf64] 串口日志：%s / %s" % (os.path.join(tmp, "boot1.log"),
                                            os.path.join(tmp, "boot2.log")))

    for tag, log in (("boot1", log1), ("boot2", log2)):
        print("--- %s serial tail ---" % tag)
        for line in [x for x in log.splitlines() if x.strip()][-16:]:
            print("   | " + line[:180])

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
