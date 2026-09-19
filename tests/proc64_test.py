#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/proc64_test.py - 批次 C：进程级地址空间 + fork/execve/wait4/kill 端到端验收

做什么（与 tests/elf64_test.py 同一套骨架，只是断言换了对象）：
  1) 造一块"已装好系统、主分区已格式化"的 16MB 测试盘（build64/system.img 的字节
     + 标准 MBR 分区表 + 在 LBA 8009 上复刻 kernel/vfs64.cpp 的空 VimtuFS2 卷）；
  2) 无头启动它，读串口断言（BIOS/SeaBIOS 路径 -> 每进程 CR3 必须生效）：
       [PROC64] init mode=isolated       引导期页表是我们自己建的 -> 每进程 CR3 打开
       [PROC64] selftest PASS            进程表/地址空间布局自检
       [PROC64] install ok path=/proc64.elf
       [PROC64] create pid=.. name=init  init 进程（自己的 CR3 + 自己的任务）
       [PROC64] fork parent=.. child=..  整页物理复制
       [PROC64] execve path=/hello.elf   子进程换映像 -> 跑 hello.elf
       [PROC64] wait4 pid=.. status=0    父进程收到"子进程 exit(0)"
       [PROC64] execve path=/proc64.elf  （带 argv：child2 / killme）
       [PROC64] wait4 pid=.. status=1792 子进程 exit(7) -> status=(7&0xFF)<<8
       [PROC64] kill pid=.. sig=15       SIGTERM
       [PROC64] exit pid=.. code=143 cr3_released=1
       [PROC64] wait4 pid=.. status=36608  ((143&0xFF)<<8)
       [PROC64] demo done .. exited=1 code=0
       [GUI64] ready                     跑完多进程演示后桌面照常起来
     用户程序侧的隔离证据（同一个 VA 各自读到自己的值）：
       proc64: parent same-va va=0x0000000100090000 value=0xaaaaaaaaaaaaaaaa marker=0x0000000000001111
       proc64: child1 same-va va=0x0000000100090000 value=0xbbbbbbbbbbbbbbbb marker=0x0000000000002222
  3) 每进程 CR3 互不相同（解析 create/fork 行里的 cr3= 十六进制，去重后 >= 4 个）+ 旧进程
     数据仍然正确（上面那两条 same-va 读回值就是"切走再切回"的证据）。
  4) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [PROC64] demo skipped /
     [PROC64] wait4 TIMEOUT / [PROC64] fork FAILED / [PROC64] execve FAILED。

第二阶段（可选，需要 OVMF）：把系统内核做成 ESP 映像在 QEMU+OVMF（UEFI）下启动，断言
  `[PROC64] init mode=shared` + `[PROC64] cr3 isolation OFF` + `[PROC64] demo skipped`
  —— UEFI 路径下引导期页表属于固件（VMware EFI 下运行期 mov cr3 已知会立刻复位），
  我们**如实降级**成共享地址空间模式，绝不假装隔离成立。找不到 OVMF 就 SKIP（不算失败）。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\proc64_test.py
    py -3 tests\\proc64_test.py --img <已装好的磁盘镜像> --timeout 150
    py -3 tests\\proc64_test.py --no-uefi        （只跑 BIOS 阶段）

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
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

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
OVMF_CANDIDATES = [
    r"C:\Program Files\qemu\share\edk2-x86_64-code.fd",
    r"C:\Program Files (x86)\qemu\share\edk2-x86_64-code.fd",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "proc64_test.img")
UEFI_IMG = os.path.join(ROOT, "build64", "proc64_uefi_test.img")
OS_KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS     # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                              # 16MB

# 第一遍启动必须出现（顺序即输出顺序）
MUST = [
    ("[PROC64] init mode=isolated",                   "引导期页表属于引导器 -> 每进程 CR3 生效"),
    ("[PROC64] selftest PASS",                        "进程表/地址空间布局自检"),
    ("[PROC64] install ok path=/proc64.elf",          "把内嵌 /proc64.elf 装进 VimtuFS2"),
    ("[PROC64] create pid=",                          "建 init 进程（自己的地址空间）"),
    ("name=init",                                     "init 进程名"),
    ("[PROC64] fork parent=",                         "fork：整页物理复制"),
    ("pages=8",                                       "fork 复制了 8 页（ELF 映像 4 页 + 栈 4 页）"),
    ("[PROC64] execve path=/hello.elf",               "子进程 execve 换映像"),
    ("hello from ELF64 (syscall insn)",               "被 execve 拉起来的子进程真的在 ring3 跑输出"),
    ("[PROC64] exit pid=",                            "子进程退出（释放地址空间）"),
    ("cr3_released=1",                                 "退出时 CR3/页表已回收"),
    ("[PROC64] wait4 pid=",                           "父进程 wait4 收到子进程"),
    ("status=0",                                      "hello.elf 的 exit(0) -> status=0"),
    ("[PROC64] execve path=/proc64.elf",              "子进程 execve 自己（带 argv）"),
    ("status=1792",                                   "子进程 exit(7) -> status=(7&0xFF)<<8 = 1792"),
    ("[PROC64] kill pid=",                            "父进程 kill 另一个子进程"),
    ("sig=15",                                        "SIGTERM"),
    ("code=143",                                      "被 SIGTERM 的进程以退出码 143 结束"),
    ("status=36608",                                  "((143&0xFF)<<8) = 36608"),
    ("[PROC64] reap pid=",                            "wait4 之后进程记录被回收（目标进程消失）"),
    ("[PROC64] demo done",                            "启动期多进程演示收尾"),
    ("exited=1 code=0",                               "init 正常退出（exit 0）"),
    ("proc64: demo done exit(0)",                     "用户程序自己打印的收尾行"),
    ("[GUI64] ready",                                 "跑完进程演示后桌面照常起来"),
]

# 用户程序里的隔离证据（同一个 VA，两个进程各自读到自己的值）
# 用户程序里的隔离证据（同一个 VA，两个进程各自读到自己的值）。
# ★ 注意：内核会把每个 write 系统调用打一行 [SYSCALL] insn ...，所以用户程序的输出在串口上
#   是**被这些行打断**的（"value=" 与具体数值不在同一行）。断言因此按**片段**匹配，并把
#   "同一 VA + 父子两套不同的值/标记"这三件事分开断言。
ISO_PARENT_PARTS = ["proc64: parent same-va va=", "0xaaaaaaaaaaaaaaaa", "0x0000000000001111"]
ISO_CHILD_PARTS  = ["proc64: child1 same-va va=", "0xbbbbbbbbbbbbbbbb", "0x0000000000002222"]
ISO_VA = "0x0000000100090000"          # 父子写的是同一个 VA（各自私有副本）
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[PROC64] demo skipped",
    "[PROC64] wait4 TIMEOUT",
    "[PROC64] fork FAILED",
    "[PROC64] execve FAILED",
    "[PROC64] start FAILED",
    "[USER64] enter FAILED",
    "[SYSCALL] deny",
    # 注意：**不要**禁 "[VFS64] read: not found" / "[VFS64] mount FAILED reason=crc" ——
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


def find_ovmf():
    for c in OVMF_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def q(p):
    return p.replace("\\", "/")


def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 测试夹具：一块"已装好系统 + 主分区已格式化"的盘（与 elf64_test.py / app64_test.py 同一套）
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
    struct.pack_into("<I", sb, 8, 2)
    struct.pack_into("<I", sb, 12, SECTOR)
    struct.pack_into("<I", sb, 16, SECTOR)
    struct.pack_into("<I", sb, 20, total_sectors)
    struct.pack_into("<I", sb, 24, 0)
    struct.pack_into("<I", sb, 28, bm_start)
    struct.pack_into("<I", sb, 32, bmn)
    struct.pack_into("<I", sb, 36, ino_start)
    struct.pack_into("<I", sb, 40, inodes)
    struct.pack_into("<I", sb, 44, 64)
    struct.pack_into("<I", sb, 48, data_start)
    struct.pack_into("<I", sb, 52, data_blocks)
    struct.pack_into("<I", sb, 56, 0)
    struct.pack_into("<I", sb, 60, _crc32(sb[:60]))
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
    root[0] = 2
    struct.pack_into("<I", root, 28, 0)
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
# UEFI 夹具：一个 ESP 分区（放 BOOTX64.EFI / UEFI64.BIN / KERNEL64.BIN / SYSTEM.IMG）
#           + 主分区（VimtuFS2）。OVMF 从 ESP 里的 BOOTX64.EFI 起，跑的是**系统内核**。
# ---------------------------------------------------------------------------
ESP_LBA = 2048
ESP_SECTORS = 14336        # 7MB

def _ovmf_vars():

    """OVMF 的可写变量卷（pflash unit=1）：每次从 QEMU 模板拷一份干净的到 build64/。"""

    # ★ 每次都从模板刷新一份变量卷：OVMF 会把上一次的引导项写进变量卷，
    #   换机器类型（q35 -> i440fx）后那些项的**设备路径**失效，固件就直接落进 EFI Shell
    #   （实测踩过）。用干净的变量卷时，OVMF 会按"可移动介质"扫描 ESP 里的
    #   /EFI/BOOT/BOOTX64.EFI —— 正好是我们 make_esp.py 的布局。
    src = os.path.join(os.path.dirname(OVMF_CANDIDATES[0]), "edk2-i386-vars.fd")
    dst = os.path.join(ROOT, "build64", "ovmf_vars.fd")
    if os.path.exists(src):
        shutil.copyfile(src, dst)
    return dst


def _mk_esp(path):
    # ★ 每次都重新生成：ESP 里的 KERNEL64.BIN 必须是**当前**系统内核（build64/kernel64_os.bin）。
    #   踩过：直接复用 build64/esp.img（ISO 用的那份装的是安装程序内核）→ UEFI 阶段跑的根本
    #   不是系统内核，更没有 proc64。
    esp = os.path.join(ROOT, "build64", "esp_proc64.img")
    if os.path.exists(esp):
        os.remove(esp)
    args = [sys.executable, os.path.join(ROOT, "tools", "make_esp.py"), esp,
            os.path.join(ROOT, "build64", "BOOTX64.EFI"),
            os.path.join(ROOT, "build64", "kernel64_os.bin"),
            os.path.join(ROOT, "build64", "system.img"),
            os.path.join(ROOT, "build64", "UEFI64.BIN")]
    r = subprocess.run(args, capture_output=True, timeout=300)
    return esp if r.returncode == 0 and os.path.exists(esp) else None


def prepare_uefi_fixture():
    if not os.path.exists(OS_KERNEL):
        return None
    esp = _mk_esp(None)
    if not esp:
        return None
    with open(esp, "rb") as f:
        esp_bytes = f.read()
    total = ESP_LBA + max(ESP_SECTORS, (len(esp_bytes) + SECTOR - 1) // SECTOR) + 8192
    total = ((total + 2047) // 2048) * 2048
    buf = bytearray(total * SECTOR)
    # ESP
    buf[ESP_LBA * SECTOR:ESP_LBA * SECTOR + len(esp_bytes)] = esp_bytes
    # 主分区（VimtuFS2）
    main_sectors = total - (ESP_LBA + ESP_SECTORS) - 1
    main_lba = ESP_LBA + ESP_SECTORS
    if main_sectors < 4096:
        return None
    _vimtufs2_format(buf, main_lba, main_sectors)
    # MBR：P1 = ESP(0xEF)，P2 = 主分区(0x07)
    buf[446:462] = _mbr_entry(True, 0xEF, ESP_LBA, ESP_SECTORS)
    buf[462:478] = _mbr_entry(False, 0x07, main_lba, main_sectors)
    buf[510], buf[511] = 0x55, 0xAA
    with open(UEFI_IMG, "wb") as f:
        f.write(buf)
    return UEFI_IMG


def boot(qemu, img, tag, logdir, timeout, ovmf=None):
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
    if ovmf:
        # ★ 用默认机器（i440fx）+ 两个 pflash 卷跑 OVMF：
        # ★ 用默认机器（i440fx）+ 两个 pflash 卷跑 OVMF：
        #   - q35 会把盘挂到 AHCI 控制器，而本内核的 ata64 是**传统 PATA**（1F0/3F6）驱动，
        #     在 AHCI 模式下读不到盘（实测：VFS mount FAILED reason=read）；
        #   - OVMF 的 edk2-x86_64-code.fd 是**固件卷**，必须 if=pflash 加载（用 -bios 会报
        #     "could not load PC BIOS"）。
        args += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % q(ovmf),
                 "-drive", "if=pflash,format=raw,unit=1,file=%s" % q(_ovmf_vars())]
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
            s = slog()
            if "[GUI64] ready" in s:
                # 演示跑完后再等一小会儿，让桌面把后续行打完
                time.sleep(2.0)
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
    ap.add_argument("--img", default=None, help="已装好的系统盘镜像；缺省则用 build64/system.img 自动造夹具")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=180, help="每遍启动的最长等待秒数")
    ap.add_argument("--no-uefi", action="store_true", help="只跑 BIOS 阶段（跳过 OVMF）")
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
        print("[proc64] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
            return 2
        img = prepare_fixture()
        if not img:
            sys.stderr.write("造测试盘失败（%s 不合法）\n" % SYSTEM_IMG)
            return 2
        print("[proc64] 测试盘已生成：%s（%d 扇区，主分区 LBA %d 已格式化）"
              % (img, TARGET_SECTORS, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_proc64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 proc64 acceptance (BIOS/SeaBIOS) ===")
    log, early = boot(qemu, img, "bios", tmp, args.timeout)

    print("--- 必须出现 ---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log)

    print("--- 用户态隔离证据（同一个 VA，父子各自读回自己的值）---")
    for k, part in enumerate(ISO_PARENT_PARTS):
        check("父进程 same-va 段 %d：%s" % (k, part), part in log)
    for k, part in enumerate(ISO_CHILD_PARTS):
        check("子进程 same-va 段 %d：%s" % (k, part), part in log)
    check("同一个 VA（%s）在父子两侧都出现（== 每进程私有）" % ISO_VA,
          log.count(ISO_VA) >= 2)

    print("--- 每进程 CR3 ---")
    cr3s = re.findall(r"cr3=([0-9A-Fa-f]{16})", log)
    uniq = sorted(set(cr3s))
    kernel_cr3 = "0000000000040000"
    user_cr3 = [c for c in uniq if c.upper() != kernel_cr3]
    check("解析到 >=4 个不同的用户进程 CR3", len(user_cr3) >= 4,
          "uniq_cr3=%d (%s)" % (len(user_cr3), ",".join(user_cr3[:6])))
    check("内核地址空间 0x0000000000040000 也在场（说明共享内核映射的那一份没被换掉）",
          kernel_cr3 in uniq)
    # fork 行里的 cr3 必须与父进程不同（父进程 cr3 出现在 create 行）
    fm = re.search(r"\[PROC64\] fork parent=(\d+) child=(\d+) cr3=([0-9A-Fa-f]{16})", log)
    cm = re.search(r"\[PROC64\] create pid=(\d+) name=init cr3=([0-9A-Fa-f]{16})", log)
    check("fork 行与 init 进程的 CR3 不同（子进程有独立的页表根）",
          bool(fm) and bool(cm) and fm.group(3).upper() != cm.group(2).upper(),
          ("init=%s child=%s" % (cm.group(2), fm.group(3))) if (fm and cm) else "")

    print("--- kill 之后目标进程消失（kill -> exit -> reap 三步齐全）---")
    km = re.search(r"\[PROC64\] kill pid=(\d+) sig=15", log)
    if km:
        kpid = km.group(1)
        check("被 kill 的 pid=%s 有 exit ... code=143 cr3_released=1" % kpid,
              ("[PROC64] exit pid=%s code=143 cr3_released=1" % kpid) in log)
        check("被 kill 的 pid=%s 有 reap（进程记录已回收）" % kpid,
              ("[PROC64] reap pid=%s" % kpid) in log)
        idx_k = log.find("[PROC64] kill pid=%s sig=15" % kpid)
        idx_e = log.find("[PROC64] exit pid=%s code=143" % kpid)
        idx_r = log.find("[PROC64] reap pid=%s" % kpid)
        check("三步顺序正确（kill -> exit -> reap）", 0 <= idx_k < idx_e < idx_r)
    else:
        check("解析到 [PROC64] kill pid=.. sig=15", False)
    # ---- 批次 B（新增断言）：in_ring3 清位缺陷根治的回归证据 ----
    # 内核启动期跑 4 轮"建 ring3 进程(/spin.elf) -> 真进 ring3 -> kill(9) -> 收尸 -> 复用同一槽"，
    # 每轮打 round=.. slot=.. enter=ok；杀掉的进程当时在 ring3 里（spin 永不退出），
    # 所以这正是会留下脏 in_ring3 位的场景。
    print("--- 批次 B：kill 后同槽复用再进 ring3（in_ring3 清位缺陷回归）---")
    reuse = re.findall(r"\[USER64\] slotreuse round=(\d+) slot=(\d+) pid=(\d+) enter=ok", log)
    check("slotreuse 循环 >= 4 轮（kill + 同槽再进 ring3）", len(reuse) >= 4,
          "rounds=%d" % len(reuse))
    reuse_slots = sorted(set(int(r[1]) for r in reuse))
    check("4 轮必须复用同一个任务槽（不动槽就是没复用到）",
          len(reuse) >= 4 and len(reuse_slots) == 1, "slots=%s" % reuse_slots)
    check("内核自证 [USER64] slotreuse PASS rounds=4",
          "[USER64] slotreuse PASS rounds=4" in log)
    check("清位钩子真的触发过（[USER64] slot release slot=.. in_ring3=1 -> cleared）",
          re.search(r"\[USER64\] slot release slot=\d+ in_ring3=1 -> cleared", log) is not None)
    check("没有任何 [USER64] enter FAILED（缺陷已根治）", "[USER64] enter FAILED" not in log)

    print("--- 禁止出现 ---")
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log)
    if early:
        print("  [!] BIOS 启动提前退出（复位/三重故障？）")

    # ---- 第二阶段：UEFI（OVMF）—— 如实降级验证 ----
    uefi_ran = False
    if not args.no_uefi:
        ovmf = find_ovmf()
        if not ovmf:
            print("--- UEFI 阶段：SKIP（找不到 OVMF：%s）---" % ", ".join(OVMF_CANDIDATES))
        else:
            print("=== UEFI（OVMF %s）阶段：固件页表 -> 必须如实降级 ===" % os.path.basename(ovmf))
            uimg = prepare_uefi_fixture()
            if not uimg:
                print("--- UEFI 阶段：SKIP（造 ESP 映像失败：需要 build64/BOOTX64.EFI / UEFI64.BIN）---")
            else:
                ulog, uearly = boot(qemu, uimg, "uefi", tmp, args.timeout, ovmf=ovmf)
                uefi_ran = True
                check("UEFI 下也进了长模式内核（[LM64] ENTERED LONG MODE）",
                      "[LM64] ENTERED LONG MODE" in ulog)
                check("UEFI 路径走的是自研 UEFI 引导器（U: 前缀标记）", "U:====" in ulog)
                check("UEFI 下用户窗口如实判定为不可用（固件页表只读）",
                      "[USER64] user window NOT available" in ulog)
                check("UEFI 下 proc64 如实降级：mode=shared", "[PROC64] init mode=shared" in ulog)
                check("UEFI 下打印 cr3 isolation OFF（绝不假装隔离成立）",
                      "[PROC64] cr3 isolation OFF" in ulog)
                check("UEFI 下多进程演示跳过（demo skipped）", "[PROC64] demo skipped" in ulog)
                check("UEFI 下 ring3 启动器一并跳过（APP64/ELF64 说明）",
                      "[APP64] ring3 launches skipped" in ulog)
                check("UEFI 下不得出现 fork（共享地址空间模式没有 fork 语义）",
                      "[PROC64] fork parent=" not in ulog)
                check("UEFI 下桌面照常起来（降级不等于变砖）", "[GUI64] ready" in ulog)
                for needle in ("PANIC", "TRIPLE FAULT"):
                    check("UEFI 下不得出现 %s" % needle, needle not in ulog)
                if uearly:
                    print("  [!] UEFI 启动提前退出")

    if args.keep:
        print("[proc64] 串口日志：%s" % os.path.join(tmp, "bios.log"))

    print("--- bios serial tail ---")
    for line in [x for x in log.splitlines() if x.strip()][-14:]:
        print("   | " + line[:170])

    print("=== RESULT: %s ===  checks=%d ok=%d%s" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c),
           ("  (uefi=%s)" % ("ran" if uefi_ran else "skipped"))))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
