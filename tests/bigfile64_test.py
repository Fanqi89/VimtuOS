#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/bigfile64_test.py - VimtuFS2 **大文件（二级间接块）** 端到端验收（批次 M）

它做什么（全自动：宿主机造盘 -> QEMU 启动 -> 终端敲命令 -> 宿主侧解析 raw 镜像）：
  阶段 1  宿主侧造两块盘（不需要跑安装向导，秒级）：
            C: = 128MB 盘：build64/system.img 平铺 + MBR（ESP@9 + 主分区 0x07@8009）+ Python 侧 v3 卷
            D: = 32MB 数据盘：mv 的 v3 夹具（/readme.txt + /docs/notes.txt）
  阶段 2  终端跑 `bigtest`：
            `bigtest 1mb`     -> 经 fd64/fs64/vfs64 写 1 MiB（模式化字节）再分块读回逐字节 + CRC 一致
            `bigtest 8mb`     -> 同上但**上限 - 512B**（= 8388096 B，跨一级 + 二级间接块）
            `bigtest limit`   -> 想在末尾长 1 字节 -> 必须被拒（-EFBIG）、大小/内容一点不变（不留半截）
            `bigtest recycle` -> 写 2 MiB / 删 x2，空闲块数必须回到基线（间接块/子块不泄漏）
            `cat /big1.bin`   -> 明确提示截断（4096 上限，不静默）
            `ls` / `fatcheck /big8.bin` -> 大小与整文件 CRC（与宿主侧重算的 CRC 比对）
            `bigtest copy /big1.bin D` -> **文件管理器粘贴用的同一段代码**（explorer64_copy_file64）
                                           跨卷复制到 D:，两边 CRC 一致
  阶段 3  **宿主侧独立核对**（关键：不信内核自己报的数）：
            a) 解析 C: 卷的 raw 镜像，沿 inode 的 直接块 -> 一级间接 -> **二级间接（128 个子块）** 走一遍；
            b) 按同一公式在宿主机重算模式字节的 CRC32，与内核报的 CRC、与**从块链里读出来的字节**逐一比对；
            c) 核对文件大小、用到的子块数、以及 D: 上复制出来的文件（也是走块链读出来再比 CRC）。
            这三步保证"文件真的落在盘上、而且是按二级间接块链存的"，不是内核内存里的假象。

边界（如实写，别把没做的说成做了）：
  * 单文件上限 **8 MiB（8388608 B）**：v3 卷 = 4 直接块 + 一级间接（128 块）+ 二级间接（128x128 块）；
    映射能力 16516 块（8456192 B），对外上限刻意取整到 16384 块（8 MiB）。
  * **v2 旧卷没有 dind 字段**：上限仍是 67584 B（脚本不构造 v2 大文件；v2 兼容由 fs_tree/真实安装盘回归覆盖）。
  * 没有稀疏文件：`seek` 过末尾再写 = 中间**补零**（fd64 的既有语义，FS 层不做空洞）。
  * `cat` 最多打 4096 B（超出明确提示）；`write FILE TEXT` 受**命令行长度**限制（不是文件上限）。
  * 上限判定/空间预检**先失败**，不会写一半；失败路径可能留下的只是"已分配的块被回收"（有断言）。
  * 本脚本用 Windows 原生 Python：py -3 tests\\bigfile64_test.py [--timeout 900] [--keep]
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

import proc64_test as p64          # noqa: E402  find_qemu
import fs_tree_test as fst         # noqa: E402  夹具（qemu_args / vimtufs3_format / 串口工具）
import multivol64_test as mv       # noqa: E402  宿主侧 v3 卷解析 + D: 盘夹具

SECTOR = 512
SYSTEM_IMG = fst.SYSTEM_IMG
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = fst.PART_MAIN_LBA                     # 8009

C_SECTORS = 262144                                    # 128MB 系统盘
C_MAIN_SECTORS = C_SECTORS - PART_MAIN_LBA            # 254135
D_SECTORS = 65536                                     # 32MB 数据盘（够 8MiB 复制）
D_PART_LBA = mv.DATA_PART_LBA                         # 8192

# ★ 与 kernel/vfs64.cpp 的 pat64 / kernel/terminal64.cpp 的 pat64b **同一个公式**（宿主独立重算）
def pat64(off):
    return (off * 31 + (off >> 8) * 7 + (off >> 16) * 11 + 0xA5) & 0xFF


def pat_bytes(off, n):
    return bytes(pat64(off + i) for i in range(n))


def expected_crc(size):
    """生成式内容的 zlib CRC32（与内核的 crc32 实现/fs64_crc32_file64 同口径）。"""
    crc = 0
    off = 0
    CH = 1 << 16
    while off < size:
        n = min(CH, size - off)
        crc = zlib.crc32(pat_bytes(off, n), crc) & 0xFFFFFFFF
        off += n
    return crc


# ---------------------------------------------------------------------------
# v3 卷的**二级间接块**解析（宿主侧独立实现；multivol64_test 的 vfs3_read 也支持它）
# ---------------------------------------------------------------------------
def inode_dind(raw, ver):
    if ver != 3:
        return 0
    return struct.unpack_from("<I", raw, 71)[0] if len(raw) >= 75 else 0


def chain_blocks(buf, vol, ino):
    """返回 (ptrs, info)：ptrs = 按顺序的 fs 块 -> 卷块号；info 记录各级是否真的用上 + 子块数。"""
    size = ino["size"]
    dind = inode_dind(mv.vfs3_inode_raw(buf, vol, ino["idx"]), vol["ver"])
    info = dict(d0=ino["d0"], d1=ino["d1"], d2=ino["d2"], d3=ino["d3"], ind=ino["ind"], dind=dind,
                children_used=0)
    ptrs = [ino["d0"], ino["d1"], ino["d2"], ino["d3"]]
    if ino["ind"]:
        p = (vol["start"] + ino["ind"]) * SECTOR
        ptrs += [mv._u32(buf, p + 4 * k) for k in range(128)]
    children = []
    if dind:
        p = (vol["start"] + dind) * SECTOR
        children = [mv._u32(buf, p + 4 * k) for k in range(128)]
        for cb in children:
            if cb == 0:
                ptrs += [0] * 128
                continue
            info["children_used"] += 1
            q = (vol["start"] + cb) * SECTOR
            ptrs += [mv._u32(buf, q + 4 * k) for k in range(128)]
    need = (size + SECTOR - 1) // SECTOR
    info["need_blocks"] = need
    info["holes"] = sum(1 for k in range(need) if k < len(ptrs) and ptrs[k] == 0)
    info["used_l2"] = need > 132
    return ptrs[:need], info


def read_via_chain(buf, vol, ino):
    ptrs, info = chain_blocks(buf, vol, ino)
    out = bytearray()
    for blk in ptrs:
        if blk == 0:
            return None, info
        out += bytes(buf[(vol["start"] + blk) * SECTOR:(vol["start"] + blk + 1) * SECTOR])
    return bytes(out[:ino["size"]]), info


# ---------------------------------------------------------------------------
def make_system_disk(path):
    """128MB 系统盘：system.img + MBR（ESP + 主分区）+ Python 侧 v3 卷（@8009）。"""
    with open(SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    if not sys_bytes or len(sys_bytes) % SECTOR or len(sys_bytes) > C_SECTORS * SECTOR:
        return None
    buf = bytearray(C_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes
    buf[446:462] = fst._mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)
    buf[462:478] = fst._mbr_entry(False, 0x07, PART_MAIN_LBA, C_MAIN_SECTORS)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    info = fst.vimtufs3_format(buf, PART_MAIN_LBA, C_MAIN_SECTORS)
    with open(path, "wb") as f:
        f.write(buf)
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_bigfile_")
    c_disk = os.path.join(tmp, "c.img")
    d_disk = os.path.join(tmp, "d.img")
    serial = os.path.join(tmp, "serial.log")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, text):
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "[TERM] unsupported"):
            check("%s：不得出现 %s" % (tag, bad), bad not in text)

    # ==================== 阶段 1：宿主侧造盘 ====================
    print("=== 阶段 1：宿主侧造盘（C: 128MB v3 / D: 32MB v3，免安装向导）===")
    c_info = make_system_disk(c_disk)
    if c_info is None:
        sys.stderr.write("造盘失败（system.img 大小/对齐不合法）\n")
        return 2
    mv.make_data_vol_disk(d_disk, sectors=D_SECTORS, part_lba=D_PART_LBA, with_content=True)
    with open(c_disk, "rb") as f:
        c_bytes = f.read()
    cvol = mv.vfs3_vol(c_bytes, PART_MAIN_LBA)
    check("C: 是合法 VimtuFS2 卷（宿主侧解析超级块）", cvol is not None and cvol["ver"] == 3,
          "version=%s inodes=%s data_start=%s" % (cvol["ver"], cvol["inon"], cvol["data"]) if cvol else "")
    check("C: 卷的 inode 记录 128 B（v3）", cvol is not None and cvol["inosz"] == 128)
    check("C: 主分区扇区数 = %d（与 MBR 一致）" % C_MAIN_SECTORS,
          cvol is not None and cvol["total"] == C_MAIN_SECTORS)

    # ==================== 阶段 2：启动 + 终端大文件操作 ====================
    print("=== 阶段 2：启动（终端）-> bigtest 1mb/8mb/limit/recycle + cat/ls/fatcheck/copy ===")
    port = fst.free_port()
    proc = subprocess.Popen(fst.qemu_args(qemu, [c_disk, d_disk], serial, port, "Vimtu64-bigfile"),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = fst.Monitor(port)
    BIG = 8388608 - SECTOR                            # 8mb 步骤写"上限 - 512B" = 8388096
    LIMIT = 8388608                                   # limit 步骤先补到**正好上限**，再多写 1 字节
    try:
        check("桌面就绪（[GUI64] ready）", fst.wait_for(serial, "[GUI64] ready", 150, proc) and
              "[GUI64] ready" in fst.slog(serial))
        log = fst.slog(serial)
        check("VimtuFS2 自检全过（[VFS64] selftest PASS，含二级间接块 bit13..15）", "[VFS64] selftest PASS" in log)
        check("大文件自检：1.5MB 跨二级间接块 + 块数记账 + 回收回基线（[VFS64] bigfile ok ... dind=1）",
              re.search(r"\[VFS64\] bigfile ok bytes=1572864 blocks=\d+ ind=1 dind=1 free_delta=\d+ "
                        r"unlink_back_to_base=1 crc=0x[0-9A-F]{8}", log) is not None)
        check("大文件自检：上限边界被拒 + 缩小重写释放块（[VFS64] limit ok）",
              "[VFS64] limit ok max=8388608" in log)
        check("D: 是 VimtuFS2 卷（[DRV64] letter=D:）",
              re.search(r"\[DRV64\] letter=D: disk=\d+ part=\d+ fs=VimtuFS2", log) is not None)
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial, proc))

        def refocus_term():
            """把终端窗口重新置顶（meta_l + 1 = 再次打开/激活终端）——丢键时靠它重试。"""
            mon.key("meta_l", wait=0.8)
            mon.key("1", wait=1.5)

        def send_cmd(cmd, pattern, timeout=240, tries=3):
            """敲一条命令并在**新出现的日志**里等 pattern；没反应就重新聚焦终端再敲一次。"""
            for _ in range(tries):
                m0 = len(fst.slog(serial))
                mon.type_line(cmd)
                t0 = time.time()
                while time.time() - t0 < timeout:
                    mm = re.search(pattern, fst.slog(serial)[m0:])
                    if mm is not None:
                        return mm
                    if proc.poll() is not None:
                        return None
                    time.sleep(0.4)
                refocus_term()
            return None

        # --- 1 MiB ---
        m1 = send_cmd("bigtest 1mb",
                      r"\[BIG64\] write path=/big1\.bin bytes=(\d+) blocks=(\d+) free_before=(\d+) "
                      r"free_after=(\d+) crc32=0x([0-9A-F]{8}) rc=0")
        got1 = m1 is not None
        check("bigtest 1mb：写 1 MiB 成功（[BIG64] write path=/big1.bin rc=0）", got1 and m1 is not None)
        check("bigtest 1mb：CRC 与宿主侧重算一致", m1 is not None and m1.group(5) == "%08X" % expected_crc(1 << 20),
              "kernel=0x%s host=0x%08X" % (m1.group(5) if m1 else "--------", expected_crc(1 << 20)))
        # 1 MiB = 2048 数据块；2048 > 132 -> 一级间接 1 + 二级间接 1 + 子块 ceil((2048-132)/128)=15
        check("bigtest 1mb：块数记账 = 2048 数据块 + 一级 + 二级 + 15 个子块（2065）",
              m1 is not None and int(m1.group(2)) == 2065, "blocks=%s" % (m1.group(2) if m1 else "?"))
        check("bigtest 1mb：空闲块正好少 2065（没多占也没漏记账）",
              m1 is not None and int(m1.group(3)) - int(m1.group(4)) == 2065)

        # --- 上限 - 512B ---
        m8 = send_cmd("bigtest 8mb",
                      r"\[BIG64\] write path=/big8\.bin bytes=(\d+) blocks=(\d+) free_before=(\d+) "
                      r"free_after=(\d+) crc32=0x([0-9A-F]{8}) rc=0", timeout=420, tries=2)
        got8 = m8 is not None
        check("bigtest 8mb：写 8388096 B（上限 - 512B）成功", got8 and m8 is not None)
        check("bigtest 8mb：文件大小 = 8388096", m8 is not None and int(m8.group(1)) == BIG)
        check("bigtest 8mb：CRC 与宿主侧重算一致",
              m8 is not None and m8.group(5) == "%08X" % expected_crc(BIG),
              "kernel=0x%s host=0x%08X" % (m8.group(5) if m8 else "--------", expected_crc(BIG)))
        check("bigtest 8mb：二级间接块真的用上了（16383 数据块 + 一级 1 + 二级 1 + 子块 127 = 16512）",
              m8 is not None and int(m8.group(2)) == 16512, "blocks=%s" % (m8.group(2) if m8 else "?"))

        # --- 上限边界 ---
        mo = send_cmd("bigtest limit",
                      r"\[BIG64\] over path=/big8\.bin rc=(\d+) size_before=(\d+) size_after=(\d+) "
                      r"partial=0 crc_ok=(\d)", timeout=420, tries=2)
        check("bigtest limit：越上限 1 字节被拒（rc>0 = 负错误码）", mo is not None and int(mo.group(1)) > 0,
              "rc=%s" % (mo.group(1) if mo else "?"))
        check("bigtest limit：大小没变（size_before == size_after == 8388608 = 上限）",
              mo is not None and int(mo.group(2)) == LIMIT and int(mo.group(3)) == LIMIT)
        check("bigtest limit：内容没变（crc_ok=1 -> 全文件重新 CRC 与期望一致）",
              mo is not None and mo.group(4) == "1")
        check("bigtest limit：被拒的就是那多写的 1 字节（fd 层 -EFBIG = 27）",
              mo is not None and int(mo.group(1)) == 27, "rc=%s" % (mo.group(1) if mo else "?"))
        check("bigtest limit：没有半截文件（partial=0）", mo is not None)

        # --- 回收 ---
        mr = send_cmd("bigtest recycle",
                      r"\[BIG64\] recycle rounds=2 write_bytes=(\d+) free_before=(\d+) free_after=(\d+) delta=(\d+)",
                      timeout=300, tries=2)
        check("bigtest recycle：写-删 x2 后空闲块回到基线（delta=0）",
              mr is not None and mr.group(4) == "0", mr.group(0) if mr else "（缺打点）")

        # --- 文件管理器的大小显示（同一个格式化函数）---
        check("文件管理器大小显示：1 MiB -> 1.0 MB、8 MiB-512B -> 8.0 MB（[BIG64] fmt，UI 同一函数）",
              re.search(r"\[BIG64\] fmt bytes=1048576 text=1\.0 MB \| bytes=8388096 text=8\.0 MB", fst.slog(serial)) is not None)

        # --- cat：明确提示截断 ---
        mc = send_cmd("cat /big1.bin", r"\[TERM\] cmd cat bytes=(\d+) total=(\d+) truncated=(\d)", timeout=120)
        check("cat 大文件：只打 4096 B 且明确标 truncated=1（不静默截断）",
              mc is not None and mc.group(1) == "4096" and mc.group(2) == "1048576" and mc.group(3) == "1",
              mc.group(0) if mc else "（缺打点）")

        # --- ls：大小显示（字节数）---
        mls = send_cmd("ls", r"\[TERM\] cmd ls entries=(\d+) bytes=(\d+)", timeout=90)
        check("ls 仍正常（[TERM] cmd ls entries=；含大文件的字节数统计）", mls is not None,
              mls.group(0) if mls else "（缺打点）")

        # --- fatcheck：整文件 CRC（与宿主侧重算的一致）---
        mf = send_cmd("fatcheck /big8.bin",
                      r"\[FAT64\] crc path=/big8\.bin size=(\d+) crc32=([0-9A-F]{16})", timeout=420, tries=2)
        check("fatcheck /big8.bin：内核算出的整文件 CRC 与宿主侧独立重算一致",
              mf is not None and int(mf.group(1)) == LIMIT and
              int(mf.group(2), 16) == expected_crc(LIMIT),
              mf.group(0) if mf else "（缺打点）")

        # --- 跨卷复制（文件管理器粘贴用的同一段代码）---
        # ★ QEMU sendkey 发不出大写字母（见 multivol64_test 的说明）：盘符用小写 'd'，内核自己转大写
        mc2 = send_cmd("bigtest copy /big1.bin d",
                       r"\[BIG64\] copy src=(\S+) dst_vol=(\d+) dst=(\S+) bytes=(\d+) crc32=(0x[0-9A-F]{8}) rc=(\d) why=(\d)",
                       timeout=240, tries=3)
        check("bigtest copy：跨卷 C: -> D: 复制 1 MiB 成功（rc=0）",
              mc2 is not None and mc2.group(6) == "0", mc2.group(0) if mc2 else "（缺打点）")
        check("bigtest copy：目标卷上文件大小 = 1048576",
              mc2 is not None and int(mc2.group(4)) == (1 << 20))
        check("bigtest copy：目标卷上 CRC 与宿主侧重算一致",
              mc2 is not None and mc2.group(5).lower() == "0x%08x" % expected_crc(1 << 20),
              mc2.group(0) if mc2 else "")

        log2 = fst.slog(serial)
        forbid("阶段2", log2)
    finally:
        fst.kill(proc)
        time.sleep(1.0)

    # ==================== 阶段 3：宿主侧解析 raw 镜像（独立核对块链）====================
    print("=== 阶段 3：宿主侧解析 raw 镜像（块链 + 内容 CRC 独立核对）===")
    with open(c_disk, "rb") as f:
        after = f.read()
    cvol_a = mv.vfs3_vol(after, PART_MAIN_LBA)
    check("重启后 C: 卷仍合法（宿主侧解析）", cvol_a is not None and cvol_a["ver"] == 3)

    ino1 = mv.vfs3_find(after, cvol_a, "big1.bin")
    ino8 = mv.vfs3_find(after, cvol_a, "big8.bin")
    check("C: 根目录上有 big1.bin（1048576 B）", ino1 is not None and ino1["size"] == (1 << 20),
          "size=%s" % (ino1["size"] if ino1 else "?"))
    check("C: 根目录上有 big8.bin（limit 之后 = 正好上限 8388608 B）", ino8 is not None and ino8["size"] == LIMIT,
          "size=%s" % (ino8["size"] if ino8 else "?"))

    if ino1 is not None:
        data1, ch1 = read_via_chain(after, cvol_a, ino1)
        check("big1.bin：块链完整（无空洞）", data1 is not None and ch1["holes"] == 0,
              "need=%d children=%d" % (ch1["need_blocks"], ch1["children_used"]))
        check("big1.bin：宿主侧沿直接块 + 一级间接读出的字节 CRC 与期望一致",
              data1 is not None and (zlib.crc32(data1) & 0xFFFFFFFF) == expected_crc(1 << 20))
        check("big1.bin：内容逐字节等于模式（前 64B + 末 64B 抽样 + 全长）",
              data1 is not None and data1[:64] == pat_bytes(0, 64) and data1[-64:] == pat_bytes((1 << 20) - 64, 64))
    if ino8 is not None:
        data8, ch8 = read_via_chain(after, cvol_a, ino8)
        check("big8.bin：**真的用了二级间接块**（inode.dind != 0）", ch8["dind"] != 0, "dind=%d" % ch8["dind"])
        check("big8.bin：二级间接块下挂的子块数 = ceil((16384-132)/128) = 127",
              ch8["children_used"] == 127, "children=%d" % ch8["children_used"])
        check("big8.bin：块链完整（无空洞）", data8 is not None and ch8["holes"] == 0)
        check("big8.bin：宿主侧沿二级间接块链读出的字节 CRC 与期望一致（不是内存假象）",
              data8 is not None and (zlib.crc32(data8) & 0xFFFFFFFF) == expected_crc(LIMIT),
              "host_chain=0x%08X" % ((zlib.crc32(data8) & 0xFFFFFFFF) if data8 else 0))
        check("big8.bin：大小/内容与内核报的尺寸一致（16384 个数据块 = 8 MiB）",
              data8 is not None and ch8["need_blocks"] == 16384)

    # --- D: 上复制出来的文件 ---
    with open(d_disk, "rb") as f:
        d_after = f.read()
    dvol_a = mv.vfs3_vol(d_after, D_PART_LBA)
    check("D: 卷仍合法（宿主侧解析）", dvol_a is not None and dvol_a["ver"] == 3)
    if dvol_a is not None:
        df = mv.vfs3_find(d_after, dvol_a, "big1.bin")
        check("D: 根目录上有复制过来的 big1.bin", df is not None, "size=%s" % (df["size"] if df else "?"))
        if df is not None:
            ddata, dch = read_via_chain(d_after, dvol_a, df)
            check("D: 上 big1.bin 内容逐字节等于模式（宿主侧沿块链读出）",
                  ddata is not None and (zlib.crc32(ddata) & 0xFFFFFFFF) == expected_crc(1 << 20))
            check("D: 上 big1.bin 与 C: 上的内容完全一致（两边宿主侧 CRC 相同）",
                  ddata is not None and ino1 is not None and ddata == read_via_chain(after, cvol_a, ino1)[0])

    # ==================== 阶段 4：冷启动第二遍，从盘上读回大文件（持久化 + CRC 一致）====================
    # 这一遍不看任何内存状态：**同一对盘重新开机**，终端 `fatcheck` 把整文件按块读一遍算 CRC。
    print("=== 阶段 4：冷启动第二遍（同一对盘）-> fatcheck 读回大文件 CRC ===")
    serial4 = os.path.join(tmp, "serial4.log")
    port4 = fst.free_port()
    proc4 = subprocess.Popen(fst.qemu_args(qemu, [c_disk, d_disk], serial4, port4, "Vimtu64-bigfile-cold"),
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon4 = fst.Monitor(port4)
    try:
        check("冷启动第二遍：桌面就绪", fst.wait_for(serial4, "[GUI64] ready", 150, proc4) and
              "[GUI64] ready" in fst.slog(serial4))
        check("冷启动第二遍：VimtuFS2 自检仍 PASS（含大文件 bit13-15）",
              "[VFS64] selftest PASS" in fst.slog(serial4))
        check("冷启动第二遍：打开终端", fst.open_terminal(mon4, serial4, proc4))
        cold_ok = True
        for (f, want_crc, want_size) in (("/big1.bin", expected_crc(1 << 20), (1 << 20)),
                                         ("/big8.bin", expected_crc(LIMIT), LIMIT)):
            got = None
            for _ in range(3):
                m0 = len(fst.slog(serial4))
                mon4.type_line("fatcheck %s" % f)
                t0 = time.time()
                while time.time() - t0 < 420:
                    mm = re.search(r"\[FAT64\] crc path=%s size=(\d+) crc32=([0-9A-F]{16})" % re.escape(f),
                                   fst.slog(serial4)[m0:])
                    if mm is not None:
                        got = mm
                        break
                    if proc4.poll() is not None:
                        break
                    time.sleep(0.5)
                if got is not None:
                    break
                mon4.key("meta_l", wait=0.8)
                mon4.key("1", wait=1.5)
            okc = (got is not None and int(got.group(1)) == want_size and int(got.group(2), 16) == want_crc)
            cold_ok = cold_ok and okc
            check("冷启动第二遍：%s 读回 size=%d crc=0x%08X（与写时一致）" % (f, want_size, want_crc), okc,
                  got.group(0) if got else "（缺打点）")
        check("冷启动第二遍：两个大文件都完整读回（CRC 一致）", cold_ok)
        log4 = fst.slog(serial4)
        forbid("阶段4", log4)
    finally:
        fst.kill(proc4)
        time.sleep(1.0)

    print("\n断言：%d 条，PASS %d 条" % (len(checks), sum(1 for _, c in checks if c)))
    if not args.keep:
        print("（临时盘：%s；加 --keep 可保留）" % tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
