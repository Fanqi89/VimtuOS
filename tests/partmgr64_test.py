#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/partmgr64_test.py - GPT 盘与"双份分区表"盘上的 删除 / 格式化（v0.5.14 硬判据）

为什么必须有这个测试
------------------
v0.5.13 及以前，向导磁盘页把 **GPT 项** 列出来，但删除/格式化只按 **MBR 项号** 写盘。
实测（本脚本场景 1 修前的原始记录，见提交信息）：
  * 选中 GPT 分区 1（LBA 2048）按"删除" -> `[PART] 删除分区 OK index=2`，清掉的是
    MBR 第 2 项（保护性 MBR 里那是空的），用户选的分区一个字节没动；
  * 按"格式化" -> `[PART] 格式化 OK index=1 start=1 sectors=262143`，即按 MBR 保护性
    0xEE 项的几何去把 **LBA1..64 清零** —— 主 GPT 头和项数组被抹掉，选的分区没动。

场景 1（Windows 风格 GPT 盘：保护性 MBR + 主 GPT(LBA1) + 备份 GPT(盘尾)，2 个分区）：
  按键：f,f（格式化 GPT 分区1 = 基本数据分区 -> 真 VimtuFS2）
        ↓ f,f（格式化 GPT 分区2 = EFI 系统分区 -> 必须被拒绝）
         f（再武装一次）→ Esc（取消，**不应该退回上一页**）
        ↑ d,d（删除 GPT 分区1）
  断言（串口）：table=gpt 的格式化/删除打点，start/sectors 与 GPT 项一致；
                ESP 被拒并有 `[PART] 拒绝格式化 EFI 系统分区`。
  断言（盘字节）：
    * LBA1 仍是合法 GPT 主头（magic + 头 CRC 自检通过）——**修前这里被清零**
    * 主 GPT 项0 全 0、项1 仍在；备份 GPT 项0 全 0、项1 仍在；两份头 CRC 都自检通过
    * 保护性 MBR 的 0xEE 项完好（修前会被当成"第 2 项"删掉）
    * 分区1 首扇区 = 全 0（删除时抹掉）且第 2 扇区 = 0xFF..（格式化写下的 VimtuFS2
      位图）—— 两件事分别证明"格式化真写到 2048"和"删除真抹掉首扇区"
    * 分区2（ESP）首扇区仍是预填的 0xA5（被拒绝，没被动过）

场景 2（本系统装好的盘的样子：MBR 项 + 盘尾备份 GPT，LBA1 是 loader 不是 GPT 头）：
  按键：↓ d,d（删除主分区）→ f,f（格式化引导分区 P1）
  断言（盘字节）：
    * MBR 第 2 项全 0，第 1 项（0xEF 引导分区）完好
    * 备份 GPT 里描述同一段的那一项也被清掉，ESP 项仍在，头 CRC 重算后仍自检通过
    * LBA1 仍是 loader 的 0xAA（**绝不能被当 GPT 头清零**）
    * 主分区首扇区被抹成 0（预填的 0x5A 没了）；引导分区首部 64 扇区清零、更深处保留

退出码：0 全过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SECT = 512
TOTAL = 262144          # 128 MB
ENT_LBA = 2
ENT_COUNT = 128
ENT_SIZE = 128
BAK_ENT_LBA = TOTAL - 33
BAK_HDR_LBA = TOTAL - 1

GUID_BASIC = bytes.fromhex("a2a0d0ebe5b9334487c068b6b72699c7")
GUID_ESP = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
DISK_GUID = bytes.fromhex("112233445566778899aabbccddeeff00")

PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = 8009
PART_ESP_SECTORS = 98304
PART_GPT_BACKUP_SECTORS = 33

GPT1_FIRST, GPT1_LAST = 2048, 100000          # 场景 1：基本数据分区
GPT2_FIRST, GPT2_LAST = 110000, 200000        # 场景 1：EFI 系统分区


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            f = shutil.which(c)
            if f:
                return f
    return None


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def make_entry(tguid, first, last, name):
    e = bytearray(128)
    e[0:16] = tguid
    e[16:32] = bytes(range(16))
    struct.pack_into("<QQQ", e, 32, first, last, 0)
    nm = name.encode("utf-16-le")[:70]
    e[56:56 + len(nm)] = nm
    return bytes(e)


def make_header(my_lba, alt_lba, ent_lba, ent_bytes):
    h = bytearray(SECT)
    h[0:8] = b"EFI PART"
    struct.pack_into("<IIII", h, 8, 0x00010000, 92, 0, 0)
    struct.pack_into("<QQQQ", h, 24, my_lba, alt_lba, 34, TOTAL - 34)
    h[56:72] = DISK_GUID
    struct.pack_into("<QIII", h, 72, ent_lba, ENT_COUNT, ENT_SIZE, crc32(ent_bytes))
    struct.pack_into("<I", h, 16, crc32(bytes(h[0:92])))
    return bytes(h)


def blank_disk(path):
    with open(path, "wb") as f:
        f.write(b"\0" * (TOTAL * SECT))


def build_gpt_disk(path):
    """Windows 风格：保护性 MBR + 主 GPT + 备份 GPT，两个分区，首扇区预填垃圾。"""
    blank_disk(path)
    ents = bytearray(ENT_COUNT * ENT_SIZE)
    ents[0:128] = make_entry(GUID_BASIC, GPT1_FIRST, GPT1_LAST, "Data")
    ents[128:256] = make_entry(GUID_ESP, GPT2_FIRST, GPT2_LAST, "EFI System")
    ents = bytes(ents)
    with open(path, "r+b") as f:
        mbr = bytearray(SECT)
        mbr[446 + 4] = 0xEE
        struct.pack_into("<II", mbr, 446 + 8, 1, TOTAL - 1)
        mbr[510] = 0x55
        mbr[511] = 0xAA
        f.seek(0)
        f.write(mbr)
        f.seek(ENT_LBA * SECT)
        f.write(ents)
        f.seek(1 * SECT)
        f.write(make_header(1, BAK_HDR_LBA, ENT_LBA, ents))
        f.seek(BAK_ENT_LBA * SECT)
        f.write(ents)
        f.seek(BAK_HDR_LBA * SECT)
        f.write(make_header(BAK_HDR_LBA, 1, BAK_ENT_LBA, ents))
        f.seek(GPT1_FIRST * SECT)
        f.write(b"\xA5" * SECT)
        f.seek(GPT2_FIRST * SECT)
        f.write(b"\xA5" * SECT)
        f.seek((GPT1_FIRST + 1) * SECT)
        f.write(b"\xA5" * SECT)          # 第 2 扇区也填垃圾，便于证明格式化写到了那里


def esp_geometry(disk_sectors):
    need = PART_MAIN_LBA + 16384 + PART_ESP_SECTORS + PART_GPT_BACKUP_SECTORS
    if disk_sectors < need:
        return None
    last_usable = disk_sectors - PART_GPT_BACKUP_SECTORS
    start = last_usable - PART_ESP_SECTORS
    return start, PART_ESP_SECTORS, start - PART_MAIN_LBA


def build_own_disk(path):
    """本系统装好的盘：MBR 两项（0xEF 引导 + 0x07 主分区）+ 盘尾备份 GPT + LBA1 是 loader。"""
    blank_disk(path)
    esp_start, esp_sectors, main_secs = esp_geometry(TOTAL)
    ents = bytearray(ENT_COUNT * ENT_SIZE)
    ents[0:128] = make_entry(GUID_BASIC, PART_MAIN_LBA, PART_MAIN_LBA + main_secs - 1, "VIMTU64 MAIN")
    ents[128:256] = make_entry(GUID_ESP, esp_start, esp_start + esp_sectors - 1, "VIMTU64 ESP")
    ents = bytes(ents)
    with open(path, "r+b") as f:
        mbr = bytearray(SECT)
        mbr[446 + 0] = 0x80
        mbr[446 + 4] = 0xEF
        struct.pack_into("<II", mbr, 446 + 8, PART_BOOT_LBA, PART_BOOT_SECS)
        mbr[462 + 4] = 0x07
        struct.pack_into("<II", mbr, 462 + 8, PART_MAIN_LBA, main_secs)
        mbr[478 + 4] = 0xEF
        struct.pack_into("<II", mbr, 478 + 8, esp_start, esp_sectors)
        mbr[510] = 0x55
        mbr[511] = 0xAA
        f.seek(0)
        f.write(mbr)
        # LBA1..8 = loader（不是 GPT 头）
        f.seek(1 * SECT)
        f.write(b"\xAA" * (8 * SECT))
        # 盘尾备份 GPT
        f.seek(BAK_ENT_LBA * SECT)
        f.write(ents)
        f.seek(BAK_HDR_LBA * SECT)
        f.write(make_header(BAK_HDR_LBA, 1, BAK_ENT_LBA, ents))
        # 预填垃圾：分区首扇区 + 引导分区深处
        f.seek(PART_BOOT_LBA * SECT)
        f.write(b"\xA5" * (64 * SECT))
        f.seek((PART_BOOT_LBA + 70) * SECT)
        f.write(b"\xA5" * SECT)
        f.seek(PART_MAIN_LBA * SECT)
        f.write(b"\x5A" * SECT)
    return main_secs


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=6)
        except OSError:
            return b""
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return b""

    def key(self, name, wait=1.3):
        self.send("sendkey %s" % name, wait=wait)


def run_wizard(qemu, target, serial_log, port, keys, dwell=1.3):
    if os.path.exists(serial_log):
        os.remove(serial_log)

    def q(p):
        return p.replace("\\", "/")

    args = [
        qemu, "-name", "VimtuOS-partmgr",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-drive", "format=raw,file=%s,index=1,media=disk" % q(target),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial_log),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(port)
    try:
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        ready = False
        for _ in range(240):
            time.sleep(0.25)
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    if "磁盘枚举完成" in f.read():
                        ready = True
                        break
            except FileNotFoundError:
                pass
        print("[partmgr] 向导就绪：%s" % ready)
        for _ in range(4):
            mon.key("ret", wait=dwell)
        for k in keys:
            print("[partmgr] key=%s" % k)
            mon.key(k, wait=dwell + 0.7 if k != "down" and k != "up" else dwell)
        time.sleep(3)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    try:
        with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def read_all(path):
    with open(path, "rb") as f:
        return f.read()


def at(raw, lba, n=512):
    return raw[lba * SECT:lba * SECT + n]


def hdr_ok(h):
    """GPT 头自检：magic + 头大小 92 + CRC32（**先把 HeaderCRC32 字段置 0** 再算）。"""
    if h[0:8] != b"EFI PART" or struct.unpack_from("<I", h, 12)[0] != 92:
        return False
    z = bytearray(h[0:92])
    stored = struct.unpack_from("<I", z, 16)[0]
    z[16:20] = b"\0\0\0\0"
    return stored == crc32(bytes(z))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5558)
    ap.add_argument("--dir", default=ROOT)
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(MEDIUM):
        sys.stderr.write("缺少安装介质 %s（先跑 build64.sh）\n" % MEDIUM)
        return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    # ================= 场景 1：Windows 风格 GPT 盘 =================
    print("=== 场景 1) GPT 盘：格式化/删除必须打在 GPT 项上（不能碰 LBA1）===")
    t1 = os.path.join(args.dir, "target-partmgr-gpt.img")
    build_gpt_disk(t1)
    log1 = run_wizard(qemu, t1, os.path.join(args.dir, "partmgr_gpt_serial.log"), args.port,
                      ["f", "f",          # 格式化 GPT 分区1（基本数据）-> VimtuFS2
                       "down",
                       "f", "f",          # 格式化 GPT 分区2（ESP）-> 拒绝
                       "f",               # 再武装一次
                       "esc",             # Esc = 只取消武装，不退回上一页
                       "up",
                       "d", "d"])         # 删除 GPT 分区1
    raw = read_all(t1)
    print("--- 串口 PART 行 ---")
    for l in log1.splitlines():
        if "[PART]" in l or "待确认" in l:
            print("   | " + l.strip()[:160])

    check("格式化 GPT 分区1 = VimtuFS2（table=gpt index=1 start=2048）",
          "[PART] 格式化 OK drive=1 table=gpt index=1 start=2048 sectors=%d fs=VimtuFS2"
          % (GPT1_LAST - GPT1_FIRST + 1) in log1)
    check("ESP 受保护、拒绝格式化（start=110000）",
          "[PART] 拒绝格式化 EFI 系统分区（UEFI 引导卷）start=110000" in log1)
    check("删除 GPT 分区1（table=gpt index=1 start=2048 wiped=1）",
          "[PART] 删除分区 OK drive=1 table=gpt index=1 start=2048 sectors=%d wiped=1"
          % (GPT1_LAST - GPT1_FIRST + 1) in log1)
    check("两步确认：删除武装过 1 次", log1.count("[SETUP] 删除待确认") == 1,
          "武装行数=%d" % log1.count("[SETUP] 删除待确认"))
    check("两步确认：格式化武装过 3 次（含被 Esc 取消的那次）",
          log1.count("[SETUP] 格式化待确认") == 3,
          "武装行数=%d" % log1.count("[SETUP] 格式化待确认"))
    check("Esc 只取消武装（[SETUP] 确认已取消 reason=esc）",
          "[SETUP] 确认已取消 reason=esc" in log1)
    # Esc 取消后必须还在磁盘页（page=4）：如果它顺手退回上一页，后面的删除按键就到不了。
    last_page = None
    for l in log1.splitlines():
        if "[SETUP] page=" in l:
            last_page = l.strip()
    check("Esc 取消后仍停在磁盘页（没有退回上一页）", last_page == "[SETUP] page=4", last_page or "（无）")

    h1 = at(raw, 1)
    check("LBA1 仍是合法 GPT 主头（magic + 头 CRC 自检通过）", hdr_ok(h1),
          "magic=%r" % h1[0:8])
    e0 = at(raw, ENT_LBA, 128)
    e1 = raw[(ENT_LBA * SECT) + 128:(ENT_LBA * SECT) + 256]
    check("主 GPT 项0（被删的那个）全 0", not any(e0))
    check("主 GPT 项1（ESP）仍在（没有被误删）", any(e1) and e1[0:16] == GUID_ESP)
    bh = at(raw, BAK_HDR_LBA)
    b0 = raw[(BAK_ENT_LBA * SECT):(BAK_ENT_LBA * SECT) + 128]
    b1 = raw[(BAK_ENT_LBA * SECT) + 128:(BAK_ENT_LBA * SECT) + 256]
    check("备份 GPT 头仍是合法头（CRC 重算后自检通过）", hdr_ok(bh), "magic=%r" % bh[0:8])
    check("备份 GPT 项0 同步清空（两份表一致）", not any(b0))
    check("备份 GPT 项1（ESP）仍在", any(b1) and b1[0:16] == GUID_ESP)
    mbr = at(raw, 0)
    check("保护性 MBR 的 0xEE 项完好（修前会被当成\"第 2 项\"删掉）",
          mbr[446 + 4] == 0xEE and struct.unpack_from("<I", mbr, 446 + 8)[0] == 1,
          "type=0x%02X start=%d" % (mbr[446 + 4], struct.unpack_from("<I", mbr, 446 + 8)[0]))
    p1 = at(raw, GPT1_FIRST)
    check("分区1 首扇区被删除抹成全 0（预填的 0xA5 与格式化写的超级块都没了）",
          p1 == b"\0" * 512, p1[:16].hex())
    p1b = at(raw, GPT1_FIRST + 1)
    check("分区1 第 2 扇区 = VimtuFS2 位图首字节 0xFF（证明格式化真写到 2048）",
          p1b[0] == 0xFF and any(p1b), "首字节=0x%02X" % p1b[0])
    p2 = at(raw, GPT2_FIRST)
    check("分区2（ESP）首扇区仍是 0xA5（被拒绝，一个字节没动）", p2 == b"\xA5" * 512,
          p2[:16].hex())
    check("全程没有 PANIC", "PANIC" not in log1)

    # ================= 场景 2：本系统自己的布局（MBR 项 + 盘尾 GPT）=================
    print("=== 场景 2) 本系统布局：删主分区要把 MBR 项和盘尾 GPT 项一起清掉 ===")
    t2 = os.path.join(args.dir, "target-partmgr-own.img")
    main_secs = build_own_disk(t2)
    log2 = run_wizard(qemu, t2, os.path.join(args.dir, "partmgr_own_serial.log"), args.port + 1,
                      ["down",
                       "d", "d",          # 删除主分区（P2）
                       "f", "f"])         # 格式化引导分区（P1，0xEF）
    raw2 = read_all(t2)
    print("--- 串口 PART 行 ---")
    for l in log2.splitlines():
        if "[PART]" in l or "待确认" in l:
            print("   | " + l.strip()[:160])

    check("删除主分区（table=mbr index=2 start=8009 wiped=1）",
          "[PART] 删除分区 OK drive=1 table=mbr index=2 start=%d sectors=%d wiped=1"
          % (PART_MAIN_LBA, main_secs) in log2)
    check("盘尾 GPT 里同段的那一项也被清掉（start=8009）",
          "[PART] GPT 删除项 index=0 start=8009" in log2)
    check("格式化引导分区（table=mbr index=1 start=9 fs=blank）",
          "[PART] 格式化 OK drive=1 table=mbr index=1 start=9 sectors=8000 fs=blank" in log2)

    mbr2 = at(raw2, 0)
    e_p1 = mbr2[446:462]
    e_p2 = mbr2[462:478]
    check("MBR 第 2 项（主分区）已清空", e_p2 == b"\0" * 16, e_p2.hex())
    check("MBR 第 1 项（0xEF 引导分区）完好（删一个不动另一个）",
          e_p1[4] == 0xEF and struct.unpack_from("<I", e_p1, 8)[0] == PART_BOOT_LBA,
          "type=0x%02X start=%d" % (e_p1[4], struct.unpack_from("<I", e_p1, 8)[0]))
    check("MBR 签名仍是 55AA", mbr2[510] == 0x55 and mbr2[511] == 0xAA)
    b0 = raw2[(BAK_ENT_LBA * SECT):(BAK_ENT_LBA * SECT) + 128]
    b1 = raw2[(BAK_ENT_LBA * SECT) + 128:(BAK_ENT_LBA * SECT) + 256]
    check("盘尾 GPT 项0（主分区）全 0", not any(b0))
    check("盘尾 GPT 项1（ESP）仍在", any(b1) and b1[0:16] == GUID_ESP)
    check("盘尾 GPT 头 CRC 重算后仍自检通过", hdr_ok(at(raw2, BAK_HDR_LBA)))
    lba1 = at(raw2, 1)
    check("LBA1 仍是 loader 的 0xAA（绝不能被当 GPT 头清零）", lba1 == b"\xAA" * 512,
          lba1[:8].hex())
    p_main = at(raw2, PART_MAIN_LBA)
    check("主分区首扇区被抹成全 0（预填的 0x5A 没了）", p_main == b"\0" * 512, p_main[:16].hex())
    p_boot = at(raw2, PART_BOOT_LBA)
    check("引导分区首部被格式化清零（0xA5 没了）", p_boot == b"\0" * 512, p_boot[:16].hex())
    p_deep = at(raw2, PART_BOOT_LBA + 70)
    check("引导分区更深处保留（只清首 64 扇区，不整盘擦）", p_deep == b"\xA5" * 512,
          "首字节=0x%02X" % p_deep[0])
    check("全程没有 PANIC", "PANIC" not in log2)

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
