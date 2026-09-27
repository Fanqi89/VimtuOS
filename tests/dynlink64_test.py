#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/dynlink64_test.py - ★ A3 下半：**动态链接**（PT_INTERP + 自研 ld.so）+ FPU/xmm 上下文 + 桌面入口

做什么（一遍启动里把三件事都测掉，最后在**桌面起来之后**用终端真跑一次）：
  1) 用 build64/system.img 造一块"已装好系统、主分区已格式化"的 16MB 测试盘
     （= 与 tests/musl64_test.py / tests/elf64_test.py 同一套夹具）；
  2) 无头启动 + 显式登录手势（tests/qemuhelp.py），读串口日志逐条断言启动期演示：
       * 内核把内嵌的四份产物幂等装进 VimtuFS2：/lib/ldvimtu.so、/lib/libfoo.so、
         /dynhello.elf、/xmmsse.elf；
       * PT_INTERP 被识别：[ELF64] interp path=/lib/ldvimtu.so base=… entry=… span=…
         且 base 落在**用户窗口**（4GiB..4GiB+1MiB）、span 是页的整数倍；
       * auxv 三条对：AT_BASE = 解释器基址、AT_ENTRY = **主程序**入口（≠ 解释器入口）、
         AT_PHDR/AT_PHNUM 与主程序一致（phnum=9）；
       * ld.so 自定位（自己的 R_X86_64_RELATIVE）：[LDSO] self tag=ok value=ldso-self；
       * DT_NEEDED 加载 .so：open/read/mmap/mprotect 四条系统调用都出现，
         且 .so 的代码页被 mprotect 成 **R|X（prot=5，不可写）**、数据页 R|W（prot=6）；
       * 三类重定位都真的生效（[DYNH] 各行的值 + [LDSO] reloc done 的计数）：
           - RELATIVE：主程序/ .so / 解释器各自的汇总计数 ≥1，[DYNH] 第一行能打出来；
           - GLOB_DAT：[LDSO] resolve sym=foo_counter kind=glob_dat val=0x…
                       [DYNH] glob_dat foo_counter=41 ptr_ok=1
           - JUMP_SLOT：[LDSO] resolve sym=foo_add kind=jump_slot val=0x…
                       [DYNH] jump_slot foo_add(1,2)=44 name_first=108
           - R_X86_64_64：[DYNH] 同一行的 ptr_ok=1（.data 里的绝对指针）
       * 初始化顺序：.so 的 DT_INIT_ARRAY 与 DT_INIT 都跑过、主程序自己的 DT_INIT 也跑过
         （[LDFOO] init_array called / [LDFOO] DT_INIT called / [DYNH] init hits …=1）；
       * 符号解析顺序（**主程序优先**）：foo_dup 在主程序与 libfoo.so 里**同名各有一份**，
         主程序走 PLT 调它 —— [LDSO] resolve sym=foo_dup kind=jump_slot val=0x1000xxxx
         必须落在主程序（4GiB..4GiB+64KiB）而不是 .so（4GiB+576KiB..1MiB）；函数返回 2
         （主程序那一份）而不是 1（.so 那一份）；
       * 初始栈原样交给主程序：[DYNH] argv0=/dynhello.elf argc=1（ld.so 没破坏它）；
       * 退出码：[PROC64] exit pid=… code=0 + [DYNLINK] done … exited=1 code=0 + [DYNLINK] PASS；
       * FPU/xmm 回归：/xmmsse.elf 起**两个真进程**交替做 SSE，各自核对 xmm0..15
         —— [XMM] pid=<a> ok=1 rounds=300 / [XMM] pid=<b> ok=1 rounds=300（两个 pid 不同、
         都是 ok=1），内核侧 [TASK64] fpu init sse=1 … / [TASK64] fpu save slot=… restore slot=…
         / [TASK64] fpu demo … PASS；
  3) 桌面起来（[GUI64] ready]）之后：开始菜单 -> 终端 -> 手敲命令
       `elfrun musl_hello.elf`（musl 静态 ELF：验证 DFS/TLS 那条真进程路径）
       `elfrun dynhello.elf`（**动态程序**也走同一个入口）
     断言 [ELF64] run cmd path=… rc=… via=proc pid=… code=… 与两个程序各自的输出原文。

禁止出现（一条都不能有）：PANIC / TRIPLE FAULT / [SYSCALL] deny / [SYSCALL] enosys /
  [ELF64] reject / [ELF64] interp reject / [ELF64] launch FAILED / [PROC64] start FAILED /
  [USER64] enter FAILED / [DYNLINK] FAILED / [DYNLINK] skipped / [TASK64] fpu demo … FAIL /
  [LDSO] FAIL / [DYNH] FAIL / selftest FAIL / FAILED mask=

如实标注的边界（**没有**假装通过的东西，详见 docs/应用层与系统调用说明.md 的 A3 下半节）：
  * 没有 lazy binding / dlopen / dlsym / TLS 动态模型（PT_TLS/DT_TLSDESC）/ vDSO /
    LD_LIBRARY_PATH / rpath / 符号版本 —— 见 user/ldso/ldso.c 文件头；
  * "代码段不可写"这条**不做真写入尝试**：本内核的用户态 #PF 直接 PANIC（不是杀进程），
    真写一下会把整机停下。所以这里改判**页表权限位**（mprotect 的 prot 参数走系统调用打点，
    由内核打印，不是用户程序自己说的）：代码页必须 R|X（prot=5）、数据页必须可写（prot=6）。
  * musl 的**动态**路径（ld-musl-x86_64.so.1）本轮没跑通，卡点见 docs（DT_GNU_HASH 之外的
    TLS 动态模型与 vDSO）；因此"用 musl 的 ld.so"没有断言，只有我们自研解释器的断言。

用法（必须 Windows 原生 Python）：
    py -3 tests\\dynlink64_test.py
    py -3 tests\\dynlink64_test.py --img <已装好的盘> --timeout 200 --no-desktop
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
FIXTURE_IMG = os.path.join(ROOT, "build64", "dynlink64_test.img")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS     # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                             # 16MB

# 用户窗口常量（kernel/usermode64.h 的唯一定义点；测试侧只用来判"落在窗口里"）
CODE_VA = 0x100000000
STACK_VA = 0x100010000
WINDOW_BYTES = 1024 * 1024
MMAP_VA = 0x100090000
MAIN_TEXT_HI = STACK_VA                            # 主程序映像区间上界（4GiB..4GiB+64KiB）

INTERP_SO = "/lib/ldvimtu.so"
LIBFOO_SO = "/lib/libfoo.so"
DYNH_ELF = "/dynhello.elf"
XMM_ELF = "/xmmsse.elf"

# 用户程序逐字节写出来的固定串（用 stream() 把系统调用跟踪行剥掉以后再比对）
DYNH_LINES = [
    b"[DYNH] hello from PIE + libfoo.so\n",
    b"[DYNH] glob_dat foo_counter=41 ptr_ok=1\n",
    b"[DYNH] jump_slot foo_add(1,2)=44 name_first=108\n",
    b"[DYNH] dup main wins call=2\n",
    b"[DYNH] init hits foo_init_array=1 foo_init=1 main_init=1\n",
    b"[DYNH] argv0=/dynhello.elf argc=1\n",
    b"[DYNH] PASS exit=0\n",
]
MUSL_HELLO_BYTES = b"[MUSL] hello from musl static ELF\n"

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[SYSCALL] deny",
    "[SYSCALL] enosys",
    "[ELF64] reject",
    "[ELF64] interp reject",
    "[ELF64] launch FAILED",
    "[ELF64] exec reject",
    "[PROC64] start FAILED",
    "[USER64] enter FAILED",
    "[DYNLINK] FAILED",
    "[DYNLINK] skipped",
    "[TASK64] fpu demo A=0 B=0 FAIL",
    "[TASK64] fpu demo spawn FAILED",
    "[LDSO] FAIL",
    "[DYNH] FAIL",
    "FAILED mask=",
    "selftest FAIL",
]

# ---- 终端按键映射（与 tests/fs_term_test.py 同一套 QEMU sendkey 名字）----
TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
}


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
# 测试夹具：一块"已装好系统 + 主分区已格式化"的盘（与 musl64_test.py 同一套）
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
                bm[k >> 3] |= 1 << (k & 7)
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


# ---------------------------------------------------------------------------
# 启动 / 串口 / 键盘
# ---------------------------------------------------------------------------
def boot(qemu, img, logdir, timeout, wait_desktop=True):
    """无头启动；返回 (proc, serial_path, mport)。等 [GUI64] ready（或超时）。"""
    serial = os.path.join(logdir, "dynlink.log")
    if os.path.exists(serial):
        os.remove(serial)
    mport = qh.free_port()
    args = [
        qemu, "-name", "Vimtu64-dynlink",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    qh.login_desktop(qh.Monitor(mport), serial, proc, timeout=min(timeout, 180))
    if wait_desktop:
        qh.wait_mark(serial, "[GUI64] ready", timeout, proc)
    return proc, serial, mport


def stream(log):
    """还原用户程序**逐字节**写出来的文本（把内核打点行剥掉）。

    为什么需要：内核在每个系统调用之后、以及每个任务的心跳/其它模块打点时都会往同一个
    串口写一行，而用户程序（ld.so / dynhello / libfoo）把一行输出分成好几次小 write ——
    于是内核的行会**插进用户行的中间**（实测：`[LDSO] load name=[SYSCALL] insn …`）。
    这里删掉两类内核行（都删到行尾，用户已写出的字节不受影响）：
      1) `[SYSCALL] insn …`（系统调用跟踪）；
      2) 其它 `[TAG] …` 内核行 —— 但**保留**用户程序自己的标签（[DYNH]/[LDFOO]/[MUSL]/
         [XMM]/[LDSO]），那是我们要逐字节比对的内容。
    """
    s = re.sub(r"\[SYSCALL\] insn[^\n]*\n", "", log)
    return re.sub(r"\[(?!DYNH|LDFOO|MUSL|XMM|LDSO)[A-Z][A-Z0-9_]*\][^\n]*\n", "", s)

def insn_rets(log, nr):
    out = []
    for m in re.finditer(r"\[SYSCALL\] insn nr=%d\b[^\n]*?ret=([0-9A-F]{16})" % nr, log):
        out.append(int(m.group(1), 16))
    return out


MMAP_ARGS_RE = r"\[SYSCALL\] insn nr=9 rdi=([0-9A-F]{16}) rsi=([0-9A-F]{16}) rdx=([0-9A-F]{16})"
MPROT_ARGS_RE = r"\[SYSCALL\] insn nr=10 rdi=([0-9A-F]{16}) rsi=([0-9A-F]{16}) rdx=([0-9A-F]{16})"


# ---------------------------------------------------------------------------
# 断言
# ---------------------------------------------------------------------------
def run_checks(log, fresh, check):
    s = stream(log)
    sb = s.encode("utf-8", "replace")

    # ---- ① 交付：四份产物幂等装进 VimtuFS2 ----
    if fresh:
        for path in (INTERP_SO, LIBFOO_SO, DYNH_ELF, XMM_ELF):
            m = re.search(r"\[ELF64\] install ok path=%s bytes=(\d+)" % re.escape(path), log)
            check("[①] 内嵌产物装进 VimtuFS2（install ok path=%s bytes=n）" % path,
                  m is not None, (m.group(0) if m else "（缺）"))
            if m:
                check("[①] %s 的字节数 > 0 且 < 64KiB" % path, 0 < int(m.group(1)) < 65536,
                      "bytes=%s" % m.group(1))
    else:
        m = re.search(r"\[ELF64\] install skipped \(exists\) %s size=(\d+)" % re.escape(INTERP_SO), log)
        check("[①] 幂等：install skipped (exists) /lib/ldvimtu.so size=n", m is not None,
              (m.group(0) if m else "（缺）"))

    # ---- ① PT_INTERP：内核识别 + 解释器基址落在用户窗口 ----
    mi = re.search(r"\[ELF64\] interp path=(/\S+) base=([0-9A-F]{16}) entry=([0-9A-F]{16}) "
                   r"span=(\d+)", log)
    check("[①] PT_INTERP 被识别并装载（[ELF64] interp path=… base=… entry=… span=…）",
          mi is not None, (mi.group(0) if mi else "（缺）"))
    base = entry_i = span = None
    if mi:
        check("[①] PT_INTERP 路径 = /lib/ldvimtu.so", mi.group(1) == INTERP_SO, mi.group(1))
        base = int(mi.group(2), 16)
        entry_i = int(mi.group(3), 16)
        span = int(mi.group(4))
        lo, hi = CODE_VA + MMAP_VA - CODE_VA, CODE_VA + WINDOW_BYTES   # mmap 区..窗口顶
        check("[①] 解释器基址落在用户窗口（4GiB+576KiB .. 4GiB+1MiB）",
              CODE_VA + 0x90000 <= base < hi, "base=0x%x" % base)
        check("[①] 解释器入口落在它自己的映像里（base <= entry < base+span）",
              base <= entry_i < base + span, "base=0x%x entry=0x%x span=%d" % (base, entry_i, span))
        check("[①] span 是 4KiB 的整数倍且 > 0", span > 0 and span % 4096 == 0, "span=%d" % span)

    # ---- ① auxv：AT_BASE/AT_ENTRY/AT_PHDR/AT_PHNUM 四条 ----
    # ★ 日志里有多条 [ELF64] auxv（每次装载都有一条：VAP64 演示 1 个程序头、静态程序 6 个…），
    #   所以要**按 AT_BASE 认领**动态程序那一条（它的 base 就是上面 interp 的基址），
    #   不能像原来那样取第一条（那条是 VAP64 的，phnum=1）。
    auxv_re = (r"\[ELF64\] auxv phdr=([0-9A-F]+) phent=(\d+) phnum=(\d+) base=([0-9A-F]+) "
               r"entry=([0-9A-F]+) random=([0-9A-F]+) secure=(\d+) pagesz=(\d+)")
    ma = None
    for cand in re.finditer(auxv_re, log):
        if mi and int(cand.group(4), 16) == base:      # base == 解释器基址 = 动态程序那一条
            ma = cand
            break
    if ma is None:
        ma = re.search(auxv_re, log)                   # 兜底：没有解释器（静态）时用第一条
    check("[①] 动态程序的 auxv 打点齐全（phdr/phent/phnum/base/entry/random/secure/pagesz）",
          ma is not None, (ma.group(0) if ma else "（缺）"))
    if ma and mi:
        at_phdr = int(ma.group(1), 16)
        at_phnum = int(ma.group(3))
        at_base = int(ma.group(4), 16)
        at_entry = int(ma.group(5), 16)
        check("[①] AT_BASE == 解释器基址（与 [ELF64] interp base= 一模一样）",
              at_base == base, "auxv=0x%x interp=0x%x" % (at_base, base))
        check("[①] AT_ENTRY == **主程序**入口（≠ 解释器入口）",
              at_entry != entry_i and CODE_VA <= at_entry < MAIN_TEXT_HI,
              "entry=0x%x interp_entry=0x%x" % (at_entry, entry_i))
        check("[①] AT_PHENT=56 且 AT_PHDR 在用户窗口（主程序程序头表）",
              ma.group(2) == "56" and CODE_VA <= at_phdr < MAIN_TEXT_HI,
              "phdr=0x%x phent=%s" % (at_phdr, ma.group(2)))
        check("[①] AT_PHNUM = 主程序的程序头表项数（9：含 PT_INTERP/PT_DYNAMIC）",
              at_phnum == 9, "phnum=%d" % at_phnum)

    # ---- ② 我们自己的动态程序：逐字节输出 + 退出码 ----
    check("[②] 主程序入口桩真的跑起来（argv0/argc 逐字节：ld.so 把初始栈原样交回）",
          b"[DYNH] argv0=/dynhello.elf argc=1\n" in sb)
    for line in DYNH_LINES:
        check("[②] [DYNH] 输出逐字节正确：%s" % line.decode("utf-8").strip(), line in sb)
    check("[②] 动态程序退出码 0（[PROC64] exit … code=0）",
          re.search(r"\[PROC64\] exit pid=\d+ code=0 cr3_released=1", log) is not None)
    check("[②] 内核侧收尸 + 打点（[DYNLINK] done … exited=1 code=0）",
          re.search(r"\[DYNLINK\] done pid=\d+ exited=1 code=0 ticks=\d+", log) is not None)
    check("[②] [DYNLINK] PASS", "[DYNLINK] PASS" in log)
    check("[②] ld.so 自己也被重定位（self tag=ok value=ldso-self）",
          "[LDSO] self tag=ok value=ldso-self" in s)

    # ---- ③ 三类重定位都真的生效 ----
    mr = re.search(r"\[LDSO\] reloc done relative=(\d+) glob_dat=(\d+) jump_slot=(\d+) "
                   r"abs64=(\d+) objs=(\d+)", s)
    check("[③] ld.so 汇总行（reloc done relative=N glob_dat=N jump_slot=N abs64=N objs=N）",
          mr is not None, (mr.group(0) if mr else "（缺）"))
    if mr:
        rel, glob, jump, abs64, objs = (int(mr.group(i)) for i in range(1, 6))
        check("[③] R_X86_64_RELATIVE 至少做了 2 条（解释器自己 2 + 主程序 1 + .so 1）",
              rel >= 2, "relative=%d" % rel)
        check("[③] R_X86_64_GLOB_DAT 至少 3 条（foo_counter/foo_ctor_hits/foo_init_hits）",
              glob >= 3, "glob_dat=%d" % glob)
        check("[③] R_X86_64_JUMP_SLOT 至少 3 条（dh_main/foo_add/foo_name_first/foo_dup 里的几条）",
              jump >= 3, "jump_slot=%d" % jump)
        check("[③] R_X86_64_64 至少 1 条（主程序 .data 里的绝对指针）",
              abs64 >= 1, "abs64=%d" % abs64)
        check("[③] 对象数 = 2（主程序 + libfoo.so）", objs == 2, "objs=%d" % objs)

    mg = re.search(r"\[LDSO\] resolve sym=foo_counter kind=glob_dat val=0x([0-9a-f]{16}) weak=(\d+)", s)
    check("[③] GLOB_DAT 解析到 .so 里的 foo_counter（val 在 .so 映像区间）",
          mg is not None and CODE_VA + 0x90000 <= int(mg.group(1), 16) < CODE_VA + WINDOW_BYTES,
          (mg.group(0) if mg else "（缺）"))
    mj = re.search(r"\[LDSO\] resolve sym=foo_add kind=jump_slot val=0x([0-9a-f]{16})", s)
    check("[③] JUMP_SLOT 解析到 .so 里的 foo_add（val 在 .so 映像区间）",
          mj is not None and CODE_VA + 0x90000 <= int(mj.group(1), 16) < CODE_VA + WINDOW_BYTES,
          (mj.group(0) if mj else "（缺）"))
    check("[③] .so 自己的 RELATIVE 生效（foo_name_first 返回 'l'=108）",
          b"name_first=108" in sb)
    check("[③] 主程序自己的 RELATIVE 生效（dh_msg_p 指向 .rodata：第一行能打出来）",
          b"[DYNH] hello from PIE + libfoo.so\n" in sb)
    check("[③] R_X86_64_64 生效（ptr_ok=1：绝对指针 == &foo_counter）",
          b"ptr_ok=1" in sb)

    # ---- ③-b 初始化顺序：.so 的 DT_INIT_ARRAY / DT_INIT，主程序的 DT_INIT ----
    # （说明：[LDSO] 那几行是 ld.so 分多次小 write 打出来的，中间会插进内核的系统调用
    #   跟踪行 —— 所以这里必须查**剥掉跟踪行之后的字节流** s，不能直接查原始日志。）
    check("[③] libfoo.so 的 DT_INIT_ARRAY 被调用（[LDFOO] init_array called + 汇总行）",
          "[LDFOO] init_array called" in s and
          re.search(r"\[LDSO\] init obj=libfoo\.so init_array=1", s) is not None)
    check("[③] libfoo.so 的 DT_INIT 被调用（[LDFOO] DT_INIT called + 汇总行）",
          "[LDFOO] DT_INIT called" in s and
          re.search(r"\[LDSO\] init obj=libfoo\.so DT_INIT=1", s) is not None)
    check("[③] 主程序自己的 DT_INIT 被调用（[LDSO] init obj=<main> DT_INIT=1）",
          re.search(r"\[LDSO\] init obj=<main> DT_INIT=1", s) is not None)
    check("[③] 初始化的顺序是依赖先、主程序最后（libfoo 的 init 行在主程序那行之前）",
          s.find("init obj=libfoo.so DT_INIT=1") < s.find("init obj=<main> DT_INIT=1"))

    # ---- ④ 符号解析顺序：主程序优先（同名符号以主程序为准）----
    md = re.search(r"\[LDSO\] resolve sym=foo_dup kind=(abs64|glob_dat|jump_slot) val=0x([0-9a-f]{16})", s)
    check("[④] 同名符号 foo_dup 被 ld.so 解析（[LDSO] resolve sym=foo_dup kind=… val=…）",
          md is not None, (md.group(0) if md else "（缺）"))
    if md:
        v = int(md.group(2), 16)
        check("[④] 解析顺序 = **主程序优先**：val 落在主程序映像（4GiB..4GiB+64KiB）而不是 .so",
              CODE_VA <= v < MAIN_TEXT_HI, "val=0x%x（.so 在 4GiB+576KiB 以上）" % v)
    check("[④] 调用返回的是主程序那一份的实现（call=2，.so 那份是 1）",
          b"[DYNH] dup main wins call=2\n" in sb)

    # ---- ⑤ .so 段权限：代码页 R|X（不可写）、数据页可写 ----
    mso = re.search(r"\[LDSO\] load name=libfoo\.so base=0x([0-9a-f]{16}) span=(\d+) rela=\d+ "
                    r"relative=\d+", s)
    check("[⑤] .so 装载打点（[LDSO] load name=libfoo.so base=… span=…）",
          mso is not None, (mso.group(0) if mso else "（缺）"))
    so_base = int(mso.group(1), 16) if mso else None
    so_span = int(mso.group(2)) if mso else 0
    prot = {}
    for m in re.finditer(MPROT_ARGS_RE, log):
        prot[int(m.group(1), 16)] = int(m.group(3), 16)          # {页地址: prot 位}
    if so_base is not None:
        check("[⑤] .so 的代码页被 mprotect 成 R|X（prot=5：可读可执行、**不可写**）",
              prot.get(so_base) == 5, "prot(0x%x)=%s" % (so_base, prot.get(so_base)))
        check("[⑤] .so 的数据页是可写的（最后一项 > base，prot 含 PROT_WRITE=2）",
              any(v & 2 for k, v in prot.items() if so_base < k < so_base + so_span + 0x1000),
              "prot=%s" % {("0x%x" % k): v for k, v in prot.items()})
        mm_va = [int(m.group(1), 16) for m in re.finditer(MMAP_ARGS_RE, log)]
        check("[⑤] .so 是按 MAP_FIXED 映射的（mmap nr=9 的 rdi = 要映射的那一页 = .so 基址）",
              so_base in mm_va, "mmap rdi=%s" % [hex(v) for v in mm_va[:5]])
    check("[⑤] 依赖按名字查找：[LDSO] need name=libfoo.so from=<main>",
          "[LDSO] need name=libfoo.so from=<main>" in s)
    # ---- ⑥ FPU/xmm 上下文：两个进程互不污染 ----
    check("[⑥] 进 ring3 前开了 SSE（[TASK64] fpu init sse=1 cr4=… cr0=…）",
          re.search(r"\[TASK64\] fpu init sse=1 cr4=[0-9a-f]{16} cr0=[0-9a-f]{16}", log) is not None)
    check("[⑥] 任务切换时保存/恢复 xmm（[TASK64] fpu save slot=a restore slot=b n=k，至少 8 条）",
          len(re.findall(r"\[TASK64\] fpu save slot=\d+ restore slot=\d+ n=\d+", log)) >= 8)
    msp = re.search(r"\[TASK64\] fpu demo spawn A=(\d+) B=(\d+)", log)
    check("[⑥] FPU 回归起了两个真进程（[TASK64] fpu demo spawn A=… B=…）",
          msp is not None and msp.group(1) != msp.group(2), (msp.group(0) if msp else "（缺）"))
    xm = re.findall(r"\[XMM\] pid=(\d+) ok=(\d+) rounds=(\d+)", log)
    check("[⑥] 两个进程各自核对 16 个 xmm（[XMM] pid=… ok=… rounds=…，两条）", len(xm) == 2,
          "xm=%s" % xm)
    if len(xm) == 2:
        check("[⑥] 两个 pid 不同（真的是两个进程交替跑）", xm[0][0] != xm[1][0],
              "%s vs %s" % (xm[0][0], xm[1][0]))
        check("[⑥] 两边都 ok=1（**xmm0..15 没有被对方污染**）",
              all(x[1] == "1" for x in xm), "xm=%s" % xm)
        check("[⑥] 两边都跑满 rounds=300", all(x[2] == "300" for x in xm), "xm=%s" % xm)
    check("[⑥] 内核侧汇总：[TASK64] fpu demo A=0 B=0 PASS",
          re.search(r"\[TASK64\] fpu demo A=0 B=0 PASS", log) is not None)

    # ---- ⑦ 退出码 / 无 PANIC 由 main() 的 FORBIDDEN 兜 ----
    mf = re.search(r"\[FPU64\]|\[TASK64\] fpu demo", log)
    check("[⑦] /xmmsse.elf 也走了真进程 + ELF 装载（[ELF64] load path=/xmmsse.elf …）",
          re.search(r"\[ELF64\] load path=/xmmsse\.elf entry=[0-9A-F]+ phnum=\d+ segs=\d+", log) is not None)

    # ---- ⑧ musl 静态程序在**同一份内核**里照旧跑（回归）----
    check("[⑧] musl 静态程序照旧跑通（逐字节 hello + errno/TLS 正常）",
          MUSL_HELLO_BYTES in sb and b"[MUSL] errno ok ENOENT=2\n" in sb)
    check("[⑧] musl 的退出码 7（[MUSL64] done … code=7）",
          re.search(r"\[MUSL64\] done pid=\d+ exited=1 code=7", log) is not None)


def desktop_checks(mon, serial, proc, check, log):
    """桌面起来之后：开始菜单 -> 终端 -> 手敲 elfrun（musl 静态 + 我们自己的动态程序）。"""
    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def type_line(text, per_key=0.16):
        for ch in text:
            if ch in TYPED_NAMES:
                mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch.isalnum():
                mon.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey 不支持这个字符：%r" % ch)
        mon.key("ret", wait=per_key + 0.15)

    opened = False
    for _ in range(3):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if "[APP] term opened" in qh.read_log(slog):
            opened = True
            break
    check("[⑨] 桌面之后打开终端（开始菜单 -> 终端：[APP] term opened）", opened)

    def wait_for_mark(needle, timeout):
        """等串口里出现 needle（终端敲命令 -> 内核跑完要几秒；不轮询会读到"还没写完"）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in slog():
                return True
            time.sleep(0.4)
        return False

    before = len(slog())
    type_line("elfrun musl_hello.elf")
    wait_for_mark("run cmd path=/musl_hello.elf", 40)
    log2 = qh.read_log(slog)
    m = re.search(r"\[ELF64\] run cmd path=/musl_hello\.elf rc=-?\d+ via=proc pid=(\d+) code=(\d+)",
                  log2)
    # rc 可能是 -1（退出码非 0 时命令行按"失败"报），关键看 code=7（musl 程序自己的退出码）
    check("[⑨] 桌面终端 `elfrun musl_hello.elf` 走真进程（[ELF64] run cmd … via=proc code=…）",
          m is not None, (m.group(0) if m else "（缺）"))
    check("[⑨] musl 静态 ELF 在桌面之后**再跑一遍**（逐字节输出）",
          MUSL_HELLO_BYTES in qh.read_log(slog).encode("utf-8", "replace")[before:])
    check("[⑨] 桌面这一跑的退出码 = 7（[ELF64] run cmd … code=7）",
          m is not None and m.group(2) == "7", (m.group(0) if m else ""))

    type_line("elfrun dynhello.elf")
    wait_for_mark("run cmd path=/dynhello.elf", 60)
    log3 = qh.read_log(slog)
    m2 = re.search(r"\[ELF64\] run cmd path=/dynhello\.elf rc=0 via=proc pid=(\d+) code=0", log3)
    check("[⑨] 同一个入口也能跑**动态程序**（[ELF64] run cmd path=/dynhello.elf rc=0 … code=0）",
          m2 is not None, (m2.group(0) if m2 else "（缺）"))
    # [DYNH] PASS exit=0 是一次 write 打出来的（在原始日志里就是连续的），所以直接数第二次出现
    check("[⑨] 动态程序在桌面之后第二次跑通（[DYNH] PASS exit=0 出现两次：启动期 + 桌面期）",
          log3.count("[DYNH] PASS exit=0") >= 2, "count=%d" % log3.count("[DYNH] PASS exit=0"))
    check("[⑨] 桌面之后没有新的 [LDSO] FAIL / interp reject",
          "[LDSO] FAIL" not in log3[before:] and "[ELF64] interp reject" not in log3[before:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None,
                    help="已装好的系统盘镜像；缺省则用 build64/system.img 自动造夹具 "
                         "build64/dynlink64_test.img")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240, help="每阶段最长等待秒数")
    ap.add_argument("--no-desktop", action="store_true", help="跳过桌面终端那一段")
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
        print("[dynlink64] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
            return 2
        img = prepare_fixture()
        if not img:
            sys.stderr.write("造测试盘失败（%s 不合法）\n" % SYSTEM_IMG)
            return 2
        strict_fixture = True
        print("[dynlink64] 测试盘已生成：%s（%d 扇区，主分区 LBA %d 已格式化）"
              % (img, TARGET_SECTORS, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_dynlink_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 A3 下半（动态链接 / FPU 上下文 / 桌面入口）acceptance ===")
    proc, serial, mport = boot(qemu, img, tmp, args.timeout, wait_desktop=not args.no_desktop)
    log = qh.read_log(serial)
    try:
        print("--- 启动期演示（PT_INTERP / ld.so / 三类重定位 / FPU 回归）---")
        run_checks(log, strict_fixture, check)
        if not args.no_desktop:
            print("--- 桌面之后：终端手敲 elfrun（musl 静态 + 动态程序）---")
            desktop_checks(qh.Monitor(mport), serial, proc, check, log)
        print("--- 禁止出现 ---")
        for needle in FORBIDDEN:
            check("不得出现 %s" % needle, needle not in qh.read_log(serial))
        check("桌面照常起来（[GUI64] ready）", "[GUI64] ready" in qh.read_log(serial))
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    if args.keep:
        print("[dynlink64] 串口日志：%s" % serial)

    print("--- serial tail ---")
    for line in [x for x in qh.read_log(serial).splitlines() if x.strip()][-18:]:
        print("   | " + line[:180])

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    if strict_fixture:
        print("    （夹具 = build64/system.img + 空 VimtuFS2 卷；四份动态链接/FPU 产物由内核内嵌，"
              "启动期装进文件系统，再从盘上读出来跑）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
