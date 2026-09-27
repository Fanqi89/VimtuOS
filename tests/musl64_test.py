#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/musl64_test.py - ★ A3：**musl 静态程序在 ring3 真跑起来**的端到端验收

做什么（骨架与 tests/elf64_test.py / app64_test.py 同一套，为了证据链可比）：
  1) 用 build64/system.img 造一块"已装好系统、主分区已格式化"的 16MB 测试盘
     （= system.img 的字节 + 标准 MBR 分区表 + 在 LBA 8009 上复刻 kernel/vfs64.cpp 的空 VimtuFS2 卷）；
  2) 无头启动两遍，读串口日志逐条断言：
     第一遍：内核把**内嵌的 musl 静态 ELF** 幂等装进 VimtuFS2（/musl_hello.elf）→
             建**真进程**（proc64）→ 由 elf64 加载器从**文件系统**读出来 → 建初始栈/auxv →
             ring3 进 musl 的 _start → musl 的 __libc_start_main → 我们的 main：
               * write(1, …) 的固定串（**逐字节比对**）
               * argc/argv（初始栈被 musl 正确读到）
               * malloc（musl mallocng：brk + mmap + mprotect）分配 + 写入 + 回读
               * clock_gettime / getrandom
               * errno（**TLS/%fs** 的硬证据：arch_prctl(ARCH_SET_FS) 真生效）
             → exit(7) → 回 ring0 → [PROC64] exit code=7 → 桌面照常起
     第二遍：install skipped (exists)（文件持久化在盘上）+ 仍然跑起来 + code=7。

断言（与 kernel/elf64.cpp、kernel/kernel64.cpp、user/apps/muslhello.c 的打点严格对应）：
  [ELF64] install ok path=/musl_hello.elf bytes=<n>        内嵌 blob 真写进 VimtuFS2
  [ELF64] load path=/musl_hello.elf entry=… phnum=… segs=… size=… rsp=… via=execve
                                                          真从盘上读 + 解析 + 装载 + 建栈（execve 路径）
  [ELF64] auxv phdr=… phent=56 phnum=… base=0x0 entry=… random=… secure=0 pagesz=4096
                                                          musl 启动期要的 auxv 逐条就位
  [PROC64] start loaded entry=… rsp=… pages=…             装进这个进程自己的地址空间
  [USER64] enter ring3 entry=… rsp=… name=muslhello       真的 iretq 进 ring3
  [MUSL] hello from musl static ELF                       用户程序 write(1, …) 的原样输出（字节比对）
  [MUSL] malloc ok bytes=64+4096 a=0x… b=0x…              malloc 真分配（地址在用户窗口内）
  [SYSCALL] insn nr=12/nr=9（brk/mmap，返回用户窗口地址）  malloc 走的就是内核的 brk/mmap 路径
  [MUSL] clock_gettime ok sec=… nsec=…                    clock_gettime(228) 不崩
  [MUSL] getrandom ok len=16 hex=<32 hex>                 getrandom(318) 不崩
  [MUSL] errno ok ENOENT=2                                errno 走 %fs（TLS 真生效）
  [PROC64] exit pid=… code=7                              退出码正确
  [MUSL64] done pid=… exited=1 code=7                     内核侧收尸 + 打点
  [GUI64] ready                                           跑完 musl 桌面照常起
  禁止：PANIC / TRIPLE FAULT / [SYSCALL] deny / [SYSCALL] enosys / [ELF64] reject /
        [ELF64] launch FAILED / [PROC64] start FAILED / [USER64] enter FAILED /
        [MUSL64] FAILED / [MUSL64] skipped / [MUSL64] TIMEOUT / selftest FAIL

用法（必须用 Windows 原生 Python；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\musl64_test.py
    py -3 tests\\musl64_test.py --img <已装好的磁盘镜像> --timeout 150

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
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
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402  （公共登录手势：ui.login.auto 默认 0）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "musl64_test.img")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS     # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                             # 16MB

MUSL_PATH = "/musl_hello.elf"

# user/apps/muslhello.c 里逐字节写出来的固定串（**必须**逐字节一致，不是"包含"就完事）
HELLO_BYTES = b"[MUSL] hello from musl static ELF\n"
ARGV_BYTES = b"[MUSL] argc=1 argv0=/musl_hello.elf\n"
ERRNO_BYTES = b"[MUSL] errno ok ENOENT=2\n"

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[SYSCALL] deny",
    "[SYSCALL] enosys",
    "[ELF64] reject",
    "[ELF64] launch FAILED",
    "[PROC64] start FAILED",
    "[USER64] enter FAILED",
    "[MUSL64] FAILED",
    "[MUSL64] skipped",
    "[MUSL64] TIMEOUT",
    "FAILED mask=",
    "selftest FAIL",
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
# 测试夹具：一块"已装好系统 + 主分区已格式化"的盘（与 elf64_test.py 同一套）
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
    """无头启动一块盘，等 [GUI64] ready（或超时/提前退出），返回 (log, early)。
    ★ 登录界面必须**显式输入**（ui.login.auto 默认 0）：挂一个 monitor 端口做登录手势。"""
    serial = os.path.join(logdir, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    mport = qh.free_port()
    args = [
        qemu, "-name", "Vimtu64-" + tag,
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
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

    # ★ 登录手势：等锁屏可交互（[LOCK64] bg blur ready）-> 回车两次（锁屏 -> 登录 -> 桌面）
    qh.login_desktop(qh.Monitor(mport), slog, proc, timeout=min(timeout, 180))

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


def insn_rets(log, nr):
    """抽出所有 `[SYSCALL] insn nr=<nr> … ret=<hex>` 的返回码（十六进制转 int）。"""
    out = []
    for m in re.finditer(r"\[SYSCALL\] insn nr=%d\b[^\n]*?ret=([0-9A-F]{16})" % nr, log):
        out.append(int(m.group(1), 16))
    return out


def nonzero_ret(log, nr):
    return any(v != 0 for v in insn_rets(log, nr))


def run_checks(log, tag, fresh, check):
    """一遍启动的全部断言（fresh=True = 全新格式化夹具 = 必须 install ok）。"""
    # ---- ① 交付 + 装载 + 进 ring3 ----
    if fresh:
        m = re.search(r"\[ELF64\] install ok path=/musl_hello\.elf bytes=(\d+)", log)
        check("[%s] 把内嵌 musl ELF 装进 VimtuFS2（install ok path=/musl_hello.elf bytes=n）" % tag,
              m is not None, (m.group(0) if m else "（缺）"))
        if m:
            check("[%s] 装进去的字节数 = 构建产物大小（>16KB 且 <64KB，落在内核装载区上限内）" % tag,
                  16384 < int(m.group(1)) < 65536, "bytes=%s" % m.group(1))
    else:
        m = re.search(r"\[ELF64\] install skipped \(exists\) /musl_hello\.elf size=(\d+)", log)
        check("[%s] 幂等：install skipped (exists) /musl_hello.elf size=n（文件持久化在盘上）" % tag,
              m is not None, (m.group(0) if m else "（缺）"))

    m = re.search(r"\[ELF64\] load path=/musl_hello\.elf entry=([0-9A-F]+) phnum=(\d+) segs=(\d+) "
                  r"size=(\d+) rsp=([0-9A-F]+) via=execve", log)
    check("[%s] 加载器从**文件系统**读出并装载（[ELF64] load … via=execve）" % tag, m is not None,
          (m.group(0) if m else "（缺）"))
    if m:
        entry = int(m.group(1), 16)
        check("[%s] 入口在用户窗口（4GiB..4GiB+1MiB）内" % tag,
              0x100000000 <= entry < 0x100000000 + 0x100000, "entry=0x%x" % entry)
        check("[%s] 程序头 ≤ 16 且段数 ≥ 2（静态 musl 映像的正常形态）" % tag,
              2 <= int(m.group(3)) <= 8 and 2 <= int(m.group(2)) <= 16, m.group(0))
        check("[%s] 映像大小 >16KB（确实是 musl 静态链接出来的那份）" % tag,
              int(m.group(4)) > 16384, "size=%s" % m.group(4))

    ma = re.search(r"\[ELF64\] auxv phdr=([0-9A-F]+) phent=(\d+) phnum=(\d+) base=([0-9A-F]+) "
                   r"entry=([0-9A-F]+) random=([0-9A-F]+) secure=(\d+) pagesz=(\d+)", log)
    check("[%s] auxv 打点齐全（phdr/phent/phnum/base/entry/random/secure/pagesz）" % tag,
          ma is not None, (ma.group(0) if ma else "（缺）"))
    if ma:
        check("[%s] AT_PHENT=56（musl 按它步进遍历程序头表）" % tag, ma.group(2) == "56")
        check("[%s] AT_PHDR/AT_ENTRY/AT_RANDOM 都是非 0 的用户地址" % tag,
              int(ma.group(1), 16) > 0 and int(ma.group(5), 16) > 0 and int(ma.group(6), 16) > 0)
        check("[%s] AT_BASE=0（静态：没有动态链接器）" % tag, int(ma.group(4), 16) == 0)
        check("[%s] AT_SECURE=0 且 AT_PAGESZ=4096（musl 才不会走降权/页大小歧路）" % tag,
              ma.group(7) == "0" and ma.group(8) == "4096")

    check("[%s] 装进真进程（[PROC64] start loaded entry=… rsp=… pages=…）" % tag,
          re.search(r"\[PROC64\] start loaded entry=0x[0-9A-F]+ rsp=0x[0-9A-F]+ pages=\d+", log) is not None)
    check("[%s] 真的进了 ring3（[USER64] enter ring3 … name=muslhello）" % tag,
          re.search(r"\[USER64\] enter ring3 entry=[0-9A-F]+ rsp=[0-9A-F]+ name=muslhello", log) is not None)
    check("[%s] ring3 入口已开 SSE（[USER64] fpu sse=1 cr4=… cr0=…）—— musl 的静态库要用 SSE" % tag,
          re.search(r"\[USER64\] fpu sse=1 cr4=[0-9A-F]{16} cr0=[0-9A-F]{16}", log) is not None)
    check("[%s] 内核侧确认挂上（[MUSL64] launch path=/musl_hello.elf pid=n）" % tag,
          re.search(r"\[MUSL64\] launch path=/musl_hello\.elf pid=\d+", log) is not None)

    # ---- ② write(1, …) 的**逐字节**输出 ----
    check("[%s] musl 程序 write(1, …) 的固定串逐字节正确" % tag, HELLO_BYTES in log.encode("utf-8", "replace"))
    check("[%s] argc/argv 逐字节正确（初始栈被 musl 读对）" % tag, ARGV_BYTES in log.encode("utf-8", "replace"))
    check("[%s] errno 走 %%fs/TLS（errno ok ENOENT=2 逐字节）" % tag, ERRNO_BYTES in log.encode("utf-8", "replace"))

    # ---- ③ malloc（brk + mmap 路径）----
    mm = re.search(r"\[MUSL\] malloc ok bytes=64\+4096 a=0x([0-9a-f]+) b=0x([0-9a-f]+)", log)
    check("[%s] malloc 分配 + 写入 + 回读成功（malloc ok bytes=64+4096）" % tag, mm is not None,
          (mm.group(0) if mm else "（缺）"))
    if mm:
        a, b = int(mm.group(1), 16), int(mm.group(2), 16)
        check("[%s] 两次 malloc 地址不同且都在用户窗口内（内核 mmap/brk 真给了页）" % tag,
              a != b and 0x100000000 < a < 0x100000000 + 0x100000 and
              0x100000000 < b < 0x100000000 + 0x100000, "a=0x%x b=0x%x" % (a, b))
    check("[%s] malloc 真走了 brk(12) 且拿到用户窗口地址" % tag, nonzero_ret(log, 12))
    check("[%s] malloc 真走了 mmap(9) 且拿到用户窗口地址" % tag, nonzero_ret(log, 9))
    check("[%s] arch_prctl(158, ARCH_SET_FS) 返回 0（TLS 的 MSR 写成功）" % tag,
          0 in insn_rets(log, 158))

    # ---- ④ 启动期/运行期调用 ----
    mc = re.search(r"\[MUSL\] clock_gettime ok sec=(\d+) nsec=(\d+)", log)
    check("[%s] clock_gettime(228) 成功且 nsec 合法" % tag,
          mc is not None and int(mc.group(2)) < 1000000000,
          (mc.group(0) if mc else "（缺）"))
    mr = re.search(r"\[MUSL\] getrandom ok len=16 hex=([0-9a-f]{32})", log)
    check("[%s] getrandom(318) 成功（16 字节十六进制）" % tag, mr is not None,
          (mr.group(0) if mr else "（缺）"))
    check("[%s] set_tid_address(218) 被调用" % tag, len(insn_rets(log, 218)) > 0)

    # ---- ⑤ 退出码 ----
    check("[%s] 用户 exit(7) 原样回内核（[USER64] back … exit_code=7）" % tag,
          re.search(r"\[USER64\] back to kernel \(ring0\) exit_code=7", log) is not None)
    check("[%s] 进程退出码 code=7（[PROC64] exit … code=7）" % tag,
          re.search(r"\[PROC64\] exit pid=\d+ code=7 cr3_released=1", log) is not None)
    check("[%s] 内核侧确认收尸（[MUSL64] done … exited=1 code=7）" % tag,
          re.search(r"\[MUSL64\] done pid=\d+ exited=1 code=7", log) is not None)

    # ---- ⑥ 无 PANIC / 无 deny（在 main 里对 FORBIDDEN 逐条断言）----
    check("[%s] 跑完 musl 后桌面照常起（[GUI64] ready）" % tag, "[GUI64] ready" in log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None,
                    help="已装好的系统盘镜像；缺省则用 build64/system.img 自动造夹具 "
                         "build64/musl64_test.img")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150, help="每遍启动的最长等待秒数")
    ap.add_argument("--boots", type=int, default=2, help="启动遍数（默认 2：首装 + 幂等/持久化）")
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
        print("[musl64] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
            return 2
        img = prepare_fixture()
        if not img:
            sys.stderr.write("造测试盘失败（%s 不合法）\n" % SYSTEM_IMG)
            return 2
        strict_fixture = True
        print("[musl64] 测试盘已生成：%s（%d 扇区，主分区 LBA %d 已格式化）"
              % (img, TARGET_SECTORS, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_musl64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 musl（A3 第一步）acceptance ===")
    logs = []
    for b in range(1, max(1, args.boots) + 1):
        fresh = (b == 1)
        tag = "boot%d" % b
        print("[musl64] 第 %d 遍启动：%s" % (b, "首装（全新格式化夹具）" if fresh else "幂等/持久化"))
        log, early = boot(qemu, img, tag, tmp, args.timeout)
        logs.append((tag, log))
        print("--- %s 断言 ---" % tag)
        run_checks(log, tag, fresh and strict_fixture, check)
        print("--- %s 禁止出现 ---" % tag)
        for needle in FORBIDDEN:
            check("[%s] 不得出现 %s" % (tag, needle), needle not in log)
        if early:
            print("  [!] %s QEMU 提前退出（复位/三重故障？）" % tag)

    if args.keep:
        print("[musl64] 串口日志：%s" % " / ".join(os.path.join(tmp, t + ".log") for t, _ in logs))

    for tag, log in logs:
        print("--- %s serial tail ---" % tag)
        for line in [x for x in log.splitlines() if x.strip()][-18:]:
            print("   | " + line[:180])

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    if strict_fixture:
        print("    （夹具 = build64/system.img + 空 VimtuFS2 卷；musl ELF 是内核内嵌的那一份，"
              "由内核装进文件系统再从盘上读出来跑）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
