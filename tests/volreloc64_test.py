#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/volreloc64_test.py - ★ v0.5.15：装完之后「系统卷 = 分区大小」**且文件内容真能读**

为什么必须有这个测试
------------------
v0.5.13 引入"安装时把系统卷扩到主分区大小"，判据只看了 `[VFS64] mount ok blocks=<大数>`
和"设置能存住"（都是扩容**之后**新分配的东西），**没检查卷里原有文件能不能读**。
实测 100 GiB 目标盘（v0.5.14 的安装器）：
```
[INSTALL] volume grow ok moved=24751 total=209608854 (102348 MiB)     ← 大小对了
[VFS64] mount ok blocks=209608854 free=209555585                     ← 挂载也成功
[VFS64] BAD block=8506 not in data area [51304,209608854)            ← 文件块号越界
[VFS64] skip corrupt inode=264 reason=ind range                      ← 一级/二级间接块指针错位
[VFS64] path component not found path=/hello.elf （/pipe64.elf /spin.elf /dynhello.elf …）
桌面 781 s 仍未就绪
```
两个真缺陷（都在 kernel/part64.cpp 的 part_volume_grow64）：
  ① **先清零新位图、之后才读旧位图** —— 旧位图正好落在被清零的区间里（旧卷 12 MiB ->
     旧位图 = 扇区 1..7），"哪些块被占用"整个丢掉；
  ② 只把 inode 区 + 数据区整体后移，**没有平移 inode 的 d0..d3/ind/dind 与各级间接块里
     存的绝对块号** —— 装完每个文件都指向错位的数据。

本脚本用**宿主侧独立解析卷**来判（不看内核自报）：
  * 逐文件把「安装介质载荷里的系统卷」与「装好的盘上的系统卷」按 sha256 比对，必须全部一致；
  * 每个文件用到的块都必须在位图里标"已用"；
  * 所有在用 inode 的 CRC32([0,124)) 必须自洽；
  * 卷的 total 必须**等于主分区扇区数**（= 磁盘扇区数 - 33(GPT 备份) - 98304(ESP) - 8009）；
  * 装好启动后不许出现 `skip corrupt inode` / `BAD block=` / `NOT-PERSISTENT`，且要进桌面。

两个场景：
  A) 8 GiB **raw** 盘：宿主侧能全量读盘，做完整逐字节比对 + 启动校验。
  B) 100 GiB **qcow2** 盘（宿主只剩 ~38 GB 空间，raw 装不下）：用 qemu-img dd 抽出盘首
     64 MiB（含超级块 + 整张位图 + inode 区 + 全部载荷文件的数据块）做同样的比对。

退出码：0 全过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import hashlib
import os
import re
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
PAYLOAD = os.path.join(ROOT, "build64", "sysdisk.img")
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
QEMU_IMG = r"C:\Program Files\qemu\qemu-img.exe"

MAIN_LBA = 8009
ESP_SECTORS = 98304
GPT_BACKUP = 33
SECT = 512
GIB = 1024 ** 3

sys.path.insert(0, HERE)
import qemuhelp as qh  # noqa: E402
import startmenu64_test as SM  # noqa: E402
import textstress64_test as T  # noqa: E402
DESKTOP_READY = getattr(qh, "DESKTOP_READY", "[GUI64] ready")


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


def q(p):
    return p.replace("\\", "/")


def log_of(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return ""


def wait_in(path, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in log_of(path):
            return True
        time.sleep(0.5)
    return False


def start(qemu, args, serial, port):
    a = [qemu, "-name", "VimtuOS-volreloc", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"] + args
    p = subprocess.Popen(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return p, SM.Monitor(port)


def install(qemu, target_args, serial, port):
    """ret x4（语言/现在安装/许可/类型）-> n（新建分区表）-> ret（安装系统）。"""
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(qemu, ["-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM)]
                      + target_args + ["-boot", "order=c"], serial, port)
    try:
        if not wait_in(serial, "磁盘枚举完成", 240):
            return None
        for key in ("ret", "ret", "ret", "ret", "n", "ret"):
            mon.key(key, wait=2.0)
        wait_in(serial, "[SETUP] 安装完成", 1800)
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()
    return log_of(serial)


# ==================== 宿主侧卷解析（独立实现，不看内核自报）====================
class Vol:
    """随机访问读取卷（8 GiB 镜像不能整个读进内存）。"""

    def __init__(self, path_or_blob, base_lba):
        if isinstance(path_or_blob, (bytes, bytearray)):
            self.f = None
            self.b = bytes(path_or_blob)
        else:
            self.f = open(path_or_blob, "rb")
            self.b = None
        self.base = base_lba * SECT
        sb = self._rd(self.base, SECT)
        if sb[:8] != b"VIMTUFS2":
            raise ValueError("magic=%r" % sb[:8])
        (self.ver, self.secsz, self.blksz, self.total, self.root, self.bm_start, self.bmn,
         self.ino_start, self.inon, self.inosz, self.data_start, self.datan, self.flags,
         self.crc) = struct.unpack_from("<14I", sb, 8)

    def _rd(self, off, n):
        if self.f is None:
            return self.b[off:off + n]
        self.f.seek(off)
        return self.f.read(n)

    def blk(self, n):
        return self._rd(self.base + n * SECT, SECT)

    def inode(self, i):
        return self._rd(self.base + self.ino_start * SECT + i * self.inosz, self.inosz)

    def close(self):
        if self.f is not None:
            self.f.close()

    def file_blocks(self, rec):
        d = [struct.unpack_from("<I", rec, 8 + 4 * k)[0] for k in range(4)]
        ind = struct.unpack_from("<I", rec, 24)[0]
        dind = struct.unpack_from("<I", rec, 71)[0]
        blocks = [x for x in d if x]
        if ind:
            blk = self.blk(ind)
            blocks += [struct.unpack_from("<I", blk, 4 * i)[0] for i in range(128)]
        if dind:
            blk = self.blk(dind)
            for c in range(128):
                ch = struct.unpack_from("<I", blk, 4 * c)[0]
                if not ch:
                    continue
                b2 = self.blk(ch)
                blocks += [struct.unpack_from("<I", b2, 4 * i)[0] for i in range(128)]
        return [b for b in blocks if b]

    def read_file(self, rec):
        size = struct.unpack_from("<I", rec, 4)[0]
        out = b"".join(self.blk(b) for b in self.file_blocks(rec))
        return out[:size]

    def files(self):
        """{path: (size, sha256)}；目录没有数据块，路径靠 parent 链拼。"""
        names = {}
        recs = {}
        for i in range(self.inon):
            rec = self.inode(i)
            if rec[0] not in (1, 2):
                continue
            recs[i] = rec
            nl = rec[1]
            names[i] = rec[40:40 + nl].decode("latin1") if 0 < nl <= 31 else ""
        out = {}
        for i, rec in recs.items():
            if rec[0] != 1:                     # 只看普通文件
                continue
            parts = []
            cur = i
            seen = 0
            while cur != self.root and seen < 20:
                parts.append(names.get(cur, ""))
                cur = struct.unpack_from("<I", recs[cur], 28)[0]
                seen += 1
            if not parts:
                continue
            path = "/" + "/".join(reversed(parts))
            data = self.read_file(rec)
            out[path] = (len(data), hashlib.sha256(data).hexdigest()[:16])
        return out

    def bitmap_used(self, blk):
        sec = self.bm_start + blk // 4096
        off = self.base + sec * SECT + (blk % 4096) // 8
        return bool(self._rd(off, 1)[0] & (1 << (blk % 8)))

    def inode_crc_ok(self, i):
        rec = self.inode(i)
        if rec[0] not in (1, 2):
            return None
        return struct.unpack_from("<I", rec, 124)[0] == (zlib.crc32(rec[:124]) & 0xFFFFFFFF)


def expected_total(disk_sectors):
    return disk_sectors - GPT_BACKUP - ESP_SECTORS - MAIN_LBA


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5570)
    ap.add_argument("--work", default=os.path.join(ROOT, "build64", "volreloc"))
    ap.add_argument("--skip-big", action="store_true", help="跳过 100 GiB 场景（省时间）")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu or not os.path.exists(MEDIUM) or not os.path.exists(PAYLOAD):
        print("跳过：qemu / 安装介质 / build64/sysdisk.img 不可用")
        return 2
    os.makedirs(args.work, exist_ok=True)

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    # 载荷里的系统卷（安装器就是把它拷到目标盘的 8009）
    payload_vol = Vol(PAYLOAD, MAIN_LBA)
    payload_files = payload_vol.files()
    print("载荷系统卷：total=%d bmn=%d inodes=%d data=%d 文件 %d 个"
          % (payload_vol.total, payload_vol.bmn, payload_vol.inon, payload_vol.data_start,
             len(payload_files)))

    def compare_volume(tag, path, disk_sectors):
        vol = Vol(path, MAIN_LBA)
        print("--- %s：total=%d inodes=%d bmn=%d data=%d ---"
              % (tag, vol.total, vol.inon, vol.bmn, vol.data_start))
        exp = expected_total(disk_sectors)
        check("%s 卷版本 v4 / 512B 块" % tag, vol.ver == 4 and vol.secsz == 512 and vol.blksz == 512,
              "ver=%d sec=%d blk=%d" % (vol.ver, vol.secsz, vol.blksz))
        check("%s 几何自洽（bmn=ceil(total/4096)、data=ino+inode块）" % tag,
              vol.bmn == (vol.total + 4095) // 4096 and
              vol.data_start == vol.ino_start + (vol.inon + 3) // 4,
              "bmn=%d data=%d ino=%d inon=%d" % (vol.bmn, vol.data_start, vol.ino_start, vol.inon))
        check("%s 系统卷 total == 主分区扇区数（卷大小=分区大小）" % tag, vol.total == exp,
              "total=%d 期望=%d" % (vol.total, exp))
        bad_crc = [i for i in range(vol.inon) if vol.inode_crc_ok(i) is False]
        check("%s 所有在用 inode 的 CRC32([0,124)) 自洽" % tag, not bad_crc,
              "坏 inode=%s" % (bad_crc[:6] if bad_crc else "无"))
        got = vol.files()
        missing = [p for p in payload_files if p not in got]
        diff = [p for p in payload_files if p in got and got[p] != payload_files[p]]
        check("%s 载荷里 %d 个文件全部存在" % (tag, len(payload_files)), not missing,
              "缺 %d 个：%s" % (len(missing), missing[:5]))
        check("%s 全部文件内容逐字节一致（sha256）" % tag, not diff,
              "不一致 %d 个：%s" % (len(diff), diff[:5]))
        not_used = []
        for i in range(vol.inon):
            rec = vol.inode(i)
            if rec[0] != 1:
                continue
            for b in vol.file_blocks(rec):
                if not vol.bitmap_used(b):
                    not_used.append((i, b))
                    break
        check("%s 每个文件用到的块在位图里都标了已用" % tag, not not_used,
              "未标记 %d 处：%s" % (len(not_used), not_used[:5]))
        return vol

    def login_robust(mon, serial, deadline=600):
        """稳健登录：锁屏首次合成可能几秒（100 GiB 卷实测首帧模糊 6 s），
        qemuhelp 的固定 1.0/1.5 s 回车会在这段忙期被 i8042 丢掉 —— 这里改成
        "反复回车直到出现桌面就绪"，每一步都看日志决定按什么键。"""
        t0 = time.time()
        while time.time() - t0 < 240:
            if "[LOCK64] lock screen shown" in log_of(serial):
                break
            time.sleep(1.0)
        while time.time() - t0 < deadline:
            s = log_of(serial)
            if DESKTOP_READY in s:
                return True
            if "[LOGIN64] login screen shown" in s and "[LOGIN64] login ok" not in s:
                mon.key("ret", wait=2.0)          # 登录按钮（无口令用户）
            else:
                mon.key("ret", wait=2.5)          # 锁屏 -> 登录界面（重复发，防丢键）
            time.sleep(2.0)
        return False

    def boot_and_check(tag, target_args, serial, port, min_blocks):
        if os.path.exists(serial):
            os.remove(serial)
        proc, mon = start(qemu, target_args + ["-boot", "order=c"], serial, port)
        try:
            up = login_robust(mon, serial)
        finally:
            mon.send("quit", wait=1.5)
            try:
                proc.wait(timeout=30)
            except Exception:
                if proc.poll() is None:
                    proc.kill()
        log = log_of(serial)
        check("%s 装好的系统进桌面（%s）" % (tag, DESKTOP_READY), up,
              (re.search(r"\[OS\] ready[^\r\n]*|\[GUI64\] ready[^\r\n]*", log) or ["未就绪"])[0])
        blocks = max([int(x) for x in
                      re.findall(r"\[VFS64\] mount ok blocks=(\d+) inodes=", log)] or [0])
        check("%s 挂上的系统卷 = 分区大小" % tag, blocks == min_blocks,
              "blocks=%d 期望=%d" % (blocks, min_blocks))
        # 注意：`[VFS64] BAD block=71 not in data area [6,64)` 是内核自检里**故意**喂的负例
        # （小几何 [6,64)），不算坏盘证据；只把别的 BAD block 行当失败。
        bad = [l for l in log.splitlines()
               if ("skip corrupt inode" in l) or ("BAD block=" in l and "[6,64)" not in l)]
        check("%s 启动日志没有坏 inode/坏块（自检负例之外无 skip corrupt inode / BAD block=）" % tag,
              not bad, (bad[0].strip()[:120] if bad else "无"))
        check("%s 不是 NOT-PERSISTENT" % tag, "NOT-PERSISTENT" not in log)
        check("%s 无 PANIC" % tag, "PANIC" not in log)

    # ==================== 场景 A：8 GiB raw ====================
    print("=== 场景 A) 8 GiB raw 盘：装完 -> 宿主侧逐文件比对 + 启动 ===")
    tgt_a = os.path.join(args.work, "target8g.img")
    if os.path.exists(tgt_a):
        os.remove(tgt_a)
    sectors_a = 8 * 1024 ** 3 // SECT
    with open(tgt_a, "wb") as f:
        f.truncate(sectors_a * SECT)
    log_i = install(qemu, ["-drive", "format=raw,file=%s,index=1,media=disk" % q(tgt_a)],
                    os.path.join(args.work, "install8g.log"), args.port)
    if not log_i or "[SETUP] 安装完成" not in log_i:
        print("  安装没完成")
        return 2
    m = re.search(r"\[INSTALL\] volume grow ok moved=(\d+) total=(\d+)", log_i)
    check("安装时打印了扩容（volume grow ok）", m is not None, m.group(0) if m else "缺失")
    m2 = re.search(r"\[INSTALL\] volume grow relocate delta=(\d+) inodes=(\d+) indirect_blocks=(\d+)",
                   log_i)
    check("扩容后做了块号重定位（relocate delta/inodes/indirect_blocks）", m2 is not None,
          m2.group(0) if m2 else "缺失")
    check("重定位确实改到了 inode（inodes>0）", bool(m2) and int(m2.group(2)) > 0,
          "inodes=%s" % (m2.group(2) if m2 else "?"))
    compare_volume("8 GiB raw", tgt_a, sectors_a)
    boot_and_check("8 GiB raw", ["-drive", "format=raw,file=%s" % q(tgt_a)],
                   os.path.join(args.work, "boot8g.log"), args.port + 1,
                   expected_total(sectors_a))

    # ==================== 场景 B：100 GiB qcow2 ====================
    if args.skip_big:
        print("=== 场景 B) 跳过（--skip-big）===")
    else:
        print("=== 场景 B) 100 GiB qcow2 盘（宿主空间不够 raw）：抽盘首 64 MiB 比对 ===")
        tgt_b = os.path.join(args.work, "target100g.qcow2")
        if os.path.exists(tgt_b):
            os.remove(tgt_b)
        subprocess.run([QEMU_IMG, "create", "-f", "qcow2", tgt_b, "%d" % (100 * GIB)],
                       check=True, capture_output=True)
        sectors_b = 100 * GIB // SECT
        log_b = install(qemu, ["-drive", "format=qcow2,file=%s,index=1,media=disk" % q(tgt_b)],
                        os.path.join(args.work, "install100g.log"), args.port + 2)
        if not log_b or "[SETUP] 安装完成" not in log_b:
            print("  100 GiB 安装没完成")
            return 2
        mb = re.search(r"\[INSTALL\] volume grow ok moved=(\d+) total=(\d+)", log_b)
        check("100 GiB：安装时打印了扩容", mb is not None, mb.group(0) if mb else "缺失")
        head = os.path.join(args.work, "v100_head.raw")
        if os.path.exists(head):
            os.remove(head)
        subprocess.run([QEMU_IMG, "dd", "-f", "qcow2", "-O", "raw", "bs=1M", "count=64",
                        "if=%s" % q(tgt_b), "of=%s" % q(head)], check=True, capture_output=True)
        # 盘首 64 MiB 里含：超级块 + 整张位图(26 MB) + inode 区 + 全部载荷文件的数据块
        compare_volume("100 GiB（盘首 64 MiB 窗口）", head, sectors_b)
        # superblock 的 total 是分区大小 -> 窗口里也能判（数据块没被截断的都参与比对）
        boot_and_check("100 GiB", ["-drive", "format=qcow2,file=%s" % q(tgt_b)],
                       os.path.join(args.work, "boot100g.log"), args.port + 3,
                       expected_total(sectors_b))

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
