#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/drv64_test.py - ★ v0.9.33：**ring3 驱动框架 + 零拷贝通道 + 看门狗**的端到端验收

要证明的东西（每一条都要有串口证据，不许"看起来能跑"）：
  ① **注册机制**：内核按"分区上的文件系统类型"找到 ext4 卷 -> 拉起 /usr/bin/ext4svc ->
     服务 shm_create + shm_map + r3drv_register 成功（`[R3DRV] register name=ext4svc …`）。
  ② **零拷贝**：AHCI 的 DMA 把盘上扇区**直接写进服务映射着的共享页**（一帧一项 PRDT），
     内核打 `[R3DRV] zerocopy … copied=0`；服务在自己地址空间里算 CRC32，
     与宿主侧对同一块镜像算出的值**逐位相等**（这是"数据真落地了"的硬证据）。
  ③ **文件系统在用户态解析**：超级块 / 组描述符 / inode / extent 树 / 目录项全在 ring3 走完，
     三个文件的 size + CRC32 与宿主侧一致。
  ④ **64 MiB 顺序读吞吐对比**：同一条通道、同一块盘、同样 64 MiB，
     只差 SQE 里置不置 COPY 位 —— 零拷贝路径没有那一次 CPU memcpy。
  ⑤ **服务可重启**：配置 crash=1 时服务首次启动故意空指针崩掉 -> 内核监督器
     `[R3DRV] service died … reason=exit` -> `[R3DRV] service restart … ok` ->
     新实例 `restarts=1` 正常干活；**整机不死**（后续还能进桌面/继续心跳）。
  ⑥ **只读安全态回退**：autostart=0 时内核只打 `autostart skipped reason=off`，
     不拉起任何驱动；认得出 ext4 但没有驱动的路径打 `mode=read-only reason=no-driver`。

怎么切换场景而不改内核：镜像里的 /etc/r3drv.conf 是纯 ASCII，测试对**磁盘镜像做等长字节替换**
（autostart=1 -> autostart=0 / bench=0 -> bench=1 / crash=0 -> crash=1）。
同一个字符串在内核的默认配置里也有一份，所以按"全部替换"处理（两份语义相同）。

用法：py -3 tests/drv64_test.py [--qemu PATH] [--timeout 300] [--keep]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build64")
SYSDISK = os.path.join(BUILD, "sysdisk.img")
EXT4SVC = os.path.join(BUILD, "ext4svc")
DRVWATCH = os.path.join(BUILD, "drvwatch")
MKFS = os.path.join(ROOT, "tools", "mkfs_ext4_win.py")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files\qemu\qemu-system-x86_64",
    "qemu-system-x86_64",
]

FX_BLOCKS = 16384          # 64 MiB：既是 ext4 夹具，也是 64 MiB 顺序读基准的源
BENCH_BYTES = 64 * 1024 * 1024


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.exists(c):
            return c
        if shutil.which(c):
            return shutil.which(c)
    return None


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def patch_conf(img_bytes, pairs):
    """等长字节替换（每个 pair 都要真的命中，否则直接失败 —— 宁可报错也不静默跳过）。"""
    data = img_bytes
    for old, new in pairs:
        assert len(old) == len(new), (old, new)
        n = data.count(old)
        if n == 0:
            raise SystemExit("镜像里找不到 %r（/etc/r3drv.conf 没被装进卷？）" % old)
        data = data.replace(old, new)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (SYSDISK, EXT4SVC, DRVWATCH, MKFS):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_r3drv_")
    fx = os.path.join(tmp, "ext4fx.img")
    print("=== 0) 生成 ext4 夹具（%d 块 = %d MiB，自校验）===" % (FX_BLOCKS, FX_BLOCKS * 4096 // (1024 * 1024)))
    rc = subprocess.call([sys.executable, MKFS, "--out", fx, "--blocks", str(FX_BLOCKS)],
                         stdout=subprocess.DEVNULL)
    if rc != 0 or not os.path.exists(fx):
        sys.stderr.write("夹具生成失败 rc=%s\n" % rc)
        return 2
    fx_bytes = open(fx, "rb").read()
    crc_lba0_4k = zlib.crc32(fx_bytes[:4096]) & 0xFFFFFFFF
    print("    夹具 %d B；卷首 4 KiB crc32=0x%08X" % (len(fx_bytes), crc_lba0_4k))

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def make_variant(name, pairs):
        """把 sysdisk.img 复制一份并按 pairs 打补丁（等长替换）。"""
        data = open(SYSDISK, "rb").read()
        if pairs:
            data = patch_conf(data, pairs)
        p = os.path.join(tmp, name)
        with open(p, "wb") as f:
            f.write(data)
        return p
    # ==================== 阶段 1：正常启动（注册 + 零拷贝 + ext4 解析）====================
    print("=== 1) 正常启动：/usr/bin/ext4svc 被内核按 ext4 卷拉起，零拷贝读真数据 ===")
    img1 = make_variant("r3drv_normal.img", [])          # 原样（夹具当第二块 AHCI 盘挂上去）
    # 第二块盘（夹具）用同一份镜像文件，通过 -device ide-hd 挂到 ahci.0
    serial1 = os.path.join(tmp, "serial_normal.log")
    if os.path.exists(serial1):
        os.remove(serial1)
    args_qemu = [
        qemu, "-name", "VimtuOS-r3drv-normal",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img1,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial1,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args_qemu, stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log1 = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log1 = read_text(serial1)
            if len(re.findall(r"\[DRVWATCH\] round=", log1)) >= 3:
                break
            if proc.poll() is not None:
                break
        time.sleep(2.5)
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try: proc.kill()
            except Exception: pass
    log1 = read_text(serial1)          # 退出后回读（缓冲）

    check("内核初始化驱动框架（[R3DRV] init … autostart=1）",
          re.search(r"\[R3DRV\] init conf=/etc/r3drv\.conf autostart=1", log1) is not None)
    check("配置从系统卷读出（[R3DRV] conf src=vfs … service=/usr/bin/ext4svc）",
          re.search(r"\[R3DRV\] conf src=vfs .*service=/usr/bin/ext4svc", log1) is not None)
    check("按分区类型探到 ext4 卷（[R3DRV] probe … proto=ext4）",
          re.search(r"\[R3DRV\] probe disk=\d+ lba=\d+ proto=ext4", log1) is not None,
          (re.search(r"\[R3DRV\] probe[^\r\n]*", log1).group(0) if re.search(r"\[R3DRV\] probe", log1) else "缺"))
    check("驱动未起来时是**只读安全态**（mode=read-only reason=no-driver）",
          re.search(r"\[R3DRV\] route disk=\d+ lba=\d+ proto=ext4 mode=read-only reason=no-driver", log1) is not None)
    check("内核拉起驱动服务（[R3DRV] start name=ext4svc … ok）",
          re.search(r"\[R3DRV\] start name=ext4svc path=/usr/bin/ext4svc .*pid=\d+ slot=\d+ ok", log1) is not None)
    check("服务注册成功（[R3DRV] register … mode=new）",
          re.search(r"\[R3DRV\] register name=ext4svc proto=ext4 disk=\d+ shm=\d+ bytes=\d+ pid=\d+ slot=\d+ mode=new", log1) is not None)
    check("路由升级为 driver（mode=driver slot=0）",
          re.search(r"\[R3DRV\] route disk=\d+ lba=\d+ proto=ext4 mode=driver slot=0", log1) is not None)
    check("服务侧确认注册（[EXT4SVC] register ret=0 restarts=0 ok）",
          re.search(r"\[EXT4SVC\] register ret=0 restarts=0 ok", log1) is not None)
    check("零拷贝 DMA 落进共享页（[R3DRV] zerocopy … copied=0）",
          re.search(r"\[R3DRV\] zerocopy disk=\d+ lba=\d+ secs=\d+ bytes=\d+ frames=\d+ copied=0", log1) is not None,
          (re.search(r"\[R3DRV\] zerocopy[^\r\n]*", log1).group(0) if re.search(r"\[R3DRV\] zerocopy", log1) else "缺"))
    check("用户态解析超级块（bs=4096 blocks=16384 inode_size=256 incompat=0x00000042 ok）",
          re.search(r"\[EXT4SVC\] sb bs=4096 blocks=%d inodes=\d+ inode_size=256 first_data=0 incompat=0x00000042 ok"
                    % FX_BLOCKS, log1) is not None,
          (re.search(r"\[EXT4SVC\] sb[^\r\n]*", log1).group(0) if re.search(r"\[EXT4SVC\] sb", log1) else "缺"))
    check("根目录 inode 2 读出（size=4096 links=3）",
          re.search(r"\[EXT4SVC\] root ino=2 size=4096 links=3 ok", log1) is not None)
    check("目录项 hello.txt（ino=13 type=1 size=13）",
          re.search(r"\[EXT4SVC\] dirent name=hello\.txt ino=13 type=1 size=13 ok", log1) is not None)
    check("文件 /hello.txt 内容 CRC 与宿主一致（0x1B00F179）",
          re.search(r"\[EXT4SVC\] file path=/hello\.txt size=13 crc=0x1B00F179 read=13 ok", log1, re.I) is not None,
          (re.search(r"\[EXT4SVC\] file path=/hello[^\r\n]*", log1).group(0) if re.search(r"\[EXT4SVC\] file path=/hello", log1) else "缺"))
    check("文件 /big.bin（100000 B）CRC 与宿主一致（0x92858800）",
          re.search(r"\[EXT4SVC\] file path=/big\.bin size=100000 crc=0x92858800 read=100000 ok", log1, re.I) is not None,
          (re.search(r"\[EXT4SVC\] file path=/big[^\r\n]*", log1).group(0) if re.search(r"\[EXT4SVC\] file path=/big", log1) else "缺"))
    check("子目录 /sub 里 data.bin（5000 B）CRC 与宿主一致（0x4D71C181）",
          re.search(r"\[EXT4SVC\] file path=/data\.bin size=5000 crc=0x4D71C181 read=5000 ok", log1, re.I) is not None,
          (re.search(r"\[EXT4SVC\] file path=/data[^\r\n]*", log1).group(0) if re.search(r"\[EXT4SVC\] file path=/data", log1) else "缺"))
    check("零拷贝落地数据校验和 == 宿主算的卷首 4 KiB CRC32（0x%08X）" % crc_lba0_4k,
          re.search(r"\[EXT4SVC\] zc-crc lba=0 len=4096 crc=0x%08X" % crc_lba0_4k, log1, re.I) is not None,
          (re.search(r"\[EXT4SVC\] zc-crc[^\r\n]*", log1).group(0) if re.search(r"\[EXT4SVC\] zc-crc", log1) else "缺"))
    check("心跳在跑（[R3DRV] beat slot=0 … beats=…）",
          re.search(r"\[R3DRV\] beat slot=0 pid=\d+ beats=\d+", log1) is not None)
    check("看门狗进程被拉起（[R3DRV] watchdog path=/usr/bin/drvwatch … ok）",
          re.search(r"\[R3DRV\] watchdog path=/usr/bin/drvwatch pid=\d+ ok", log1) is not None)
    check("看门狗通过 /proc/vimtu/drivers 看到服务 RUN（st=2）",
          re.search(r"\[DRVWATCH\] slot0 name=ext4svc proto=1 disk=\d+ st=2 pid=\d+", log1) is not None,
          (re.search(r"\[DRVWATCH\] slot0[^\r\n]*", log1).group(0) if re.search(r"\[DRVWATCH\] slot0", log1) else "缺"))
    check("看门狗报的 dma 字节数 > 0（合成节点是真数据，不是占位）",
          any(int(m) > 0 for m in re.findall(r"\[DRVWATCH\] slot0[^\r\n]*?dma=(\d+)", log1)),
          (re.findall(r"\[DRVWATCH\] slot0[^\r\n]*?dma=(\d+)", log1)[-1:] or ["缺"])[0])
    check("全程无 PANIC", "PANIC" not in log1)

    # ==================== 阶段 2：64 MiB 吞吐对比（零拷贝 vs 拷贝对照）====================
    print("=== 2) 64 MiB 顺序读：零拷贝路径 vs 拷贝对照路径（只差 SQE 的 COPY 位）===")
    img2 = make_variant("r3drv_bench.img", [(b"bench=0", b"bench=1")])
    serial2 = os.path.join(tmp, "serial_bench.log")
    args2 = [
        qemu, "-name", "VimtuOS-r3drv-bench",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img2,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial2,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args2, stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log2 = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log2 = read_text(serial2)
            if re.search(r"\[EXT4SVC\] bench bytes=\d+ mode=copy", log2):
                break
            if proc.poll() is not None:
                break
        time.sleep(2.0)
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try: proc.kill()
            except Exception: pass
    log2 = read_text(serial2)

    m_zc = re.search(r"\[EXT4SVC\] bench bytes=(\d+) mode=zerocopy ms=(\d+) mbs=(\d+)", log2)
    m_cp = re.search(r"\[EXT4SVC\] bench bytes=(\d+) mode=copy ms=(\d+) mbs=(\d+)", log2)
    check("零拷贝 64 MiB 顺序读跑完（bench mode=zerocopy bytes=67108864）",
          m_zc is not None and int(m_zc.group(1)) == BENCH_BYTES,
          m_zc.group(0) if m_zc else (re.search(r"\[EXT4SVC\] bench[^\r\n]*", log2).group(0)
                                      if re.search(r"\[EXT4SVC\] bench", log2) else "缺"))
    check("对照路径 64 MiB 顺序读跑完（bench mode=copy bytes=67108864）",
          m_cp is not None and int(m_cp.group(1)) == BENCH_BYTES,
          m_cp.group(0) if m_cp else "缺")
    if m_zc and m_cp:
        zc_mbs, cp_mbs = int(m_zc.group(3)), int(m_cp.group(3))
        print("    吞吐（仅作数据，不当判据）：零拷贝 %d MB/s / 对照 %d MB/s（比值 %.2f）"
              % (zc_mbs, cp_mbs, (zc_mbs / cp_mbs) if cp_mbs else 0.0))

    # ★ 判据选择（重要）：**不**拿吞吐当"零拷贝更快"的证据。
    #   实测三次 85/91/85 vs 80/83/83 MB/s，但也出现过 **82 vs 88（反向）** —— 在 QEMU/TCG 下
    #   AHCI 命令的模拟开销远大于真实硬件，把 memcpy 的差距压平了（见 docs/用户态驱动框架.md §6）。
    #   硬证据改成**确定性**的一条：内核自己报告它拷了多少字节
    #   （零拷贝路径 copied=0；对照路径 copied>0）—— 这条不受计时抖动影响。
    zc_zero = re.findall(r"\[R3DRV\] zerocopy disk=\d+ lba=\d+ secs=\d+ bytes=\d+ frames=\d+ copied=0", log2)
    cp_copy = re.findall(r"\[R3DRV\] copy disk=\d+ lba=\d+ secs=\d+ bytes=(\d+) copied=(\d+)", log2)
    check("零拷贝路径：内核始终报 copied=0（一个字节都没拷）",
          len(zc_zero) >= 1, "命中 %d 条" % len(zc_zero))
    check("对照路径：内核如实报 copied>0（它确实拷了）",
          len(cp_copy) >= 1 and all(int(m[1]) > 0 for m in cp_copy),
          (str(cp_copy[0]) if cp_copy else "缺"))
    check("两条路径读到的字节数相同（67108864）—— 差异只在“谁做那次拷贝”",
          m_zc is not None and m_cp is not None and m_zc.group(1) == m_cp.group(1) == str(BENCH_BYTES))
    check("阶段 2 无 PANIC", "PANIC" not in log2)

    # ==================== 阶段 3：服务崩溃 -> 内核监督器重新拉起 ====================
    print("=== 3) 驱动崩溃重启：crash=1 时首次启动故意空指针，监督器必须把它拉回来 ===")
    img3 = make_variant("r3drv_crash.img", [(b"crash=0", b"crash=1")])
    serial3 = os.path.join(tmp, "serial_crash.log")
    args3 = [
        qemu, "-name", "VimtuOS-r3drv-crash",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img3,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial3,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args3, stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log3 = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log3 = read_text(serial3)
            if re.search(r"\[EXT4SVC\] register ret=0 restarts=1 ok", log3):
                break
            if proc.poll() is not None:
                break
        time.sleep(3.0)
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try: proc.kill()
            except Exception: pass
    log3 = read_text(serial3)

    check("服务首次启动故意崩溃（[EXT4SVC] crash test deliberately faulting）",
          "[EXT4SVC] crash test deliberately faulting" in log3)
    check("内核监督器发现服务没了（[R3DRV] service died … reason=exit）",
          re.search(r"\[R3DRV\] service died name=ext4svc pid=\d+ beats=\d+ restarts=0 reason=", log3) is not None,
          (re.search(r"\[R3DRV\] service died[^\r\n]*", log3).group(0) if re.search(r"\[R3DRV\] service died", log3) else "缺"))
    check("内核把它重新拉起（[R3DRV] service restart … attempt=1 … ok）",
          re.search(r"\[R3DRV\] service restart name=ext4svc attempt=1 pid=\d+ ok", log3) is not None,
          (re.search(r"\[R3DRV\] service restart[^\r\n]*", log3).group(0) if re.search(r"\[R3DRV\] service restart", log3) else "缺"))
    check("新实例重新注册并带着 restarts=1（不再崩）",
          re.search(r"\[EXT4SVC\] register ret=0 restarts=1 ok", log3) is not None)
    check("重启后仍然工作了（新实例把 ext4 又解析了一遍）",
          len(re.findall(r"\[EXT4SVC\] file path=/hello\.txt", log3)) >= 1)
    check("整机没被拖垮（之后仍有 [R3DRV] beat / serve 打点）",
          re.search(r"\[R3DRV\] beat slot=0", log3) is not None)
    # ★ v0.9.34（判据加强，不换主题）：空指针这一档也要给出**延迟**证据，
    #   而且必须是内核打出来的 lat_ms 而不是主机侧猜的时间。
    m3 = re.search(r"\[R3DRV\] service restart name=ext4svc attempt=1 pid=\d+ ok lat_ms=(\d+)", log3)
    check("阶段 3 也带延迟证据并在 1 秒内（restart … ok lat_ms<=1000）",
          m3 is not None and int(m3.group(1)) <= 1000,
          ("lat_ms=%s" % m3.group(1)) if m3 else "缺 lat_ms（内核没打）")
    check("阶段 3 无 PANIC", "PANIC" not in log3)

    # ==================== 阶段 4：autostart=0 -> 只读安全态，不拉起驱动 ====================
    print("=== 4) 关掉 autostart：应当只打 skipped，不启动任何驱动进程 ===")
    img4 = make_variant("r3drv_off.img", [(b"autostart=1", b"autostart=0")])
    serial4 = os.path.join(tmp, "serial_off.log")
    args4 = [
        qemu, "-name", "VimtuOS-r3drv-off",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img4,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial4,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args4, stdout=subprocess.DEVNULL, stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log4 = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log4 = read_text(serial4)
            if "[R3DRV] autostart skipped reason=off" in log4:
                time.sleep(6.0)          # 再多等一会儿，确认真的没有服务被拉起
                break
            if proc.poll() is not None:
                break
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try: proc.kill()
            except Exception: pass
    log4 = read_text(serial4)

    check("配置读到了 autostart=0（[R3DRV] init … autostart=0）",
          re.search(r"\[R3DRV\] init conf=/etc/r3drv\.conf autostart=0", log4) is not None)
    check("明确跳过（autostart skipped reason=off）",
          "[R3DRV] autostart skipped reason=off" in log4)
    check("没有拉起任何驱动服务（无 [R3DRV] start / 无 [EXT4SVC]）",
          ("[R3DRV] start " not in log4) and ("[EXT4SVC]" not in log4))
    check("阶段 4 无 PANIC", "PANIC" not in log4)

    # ==================== 阶段 5：监督器极限 —— 真硬件异常（除零 #DE）+ 1 秒内拉起 ====================
    # 与阶段 3 的区别（判据**加强**，不是重复）：
    #   ① 崩溃原因是**真异常 #DE（除零）**，不是写空指针：#DE 走的是同一条
    #      "isr -> sig64_user_fault64 -> 默认动作杀该进程 -> p64_exit64" 链，但 int_no/名字不同，
    #      所以必须看到 `[SIG64] fault ... no=0 #DE`（证明是异常而非 exit()）。
    #   ② 不再只看"有没有重启"，而是看**多久**重启：内核在 died/restart 两行里都打 lat_ms
    #      （死亡时刻由退出漏斗即时打戳，PIT 250Hz -> 1 tick = 4ms），要求 <= 1000ms。
    print("=== 5) 监督器极限：crash=2 让服务真的除零崩溃，1 秒内必须拉起且整机不死 ===")
    img5 = make_variant("r3drv_div0.img", [(b"crash=0", b"crash=2")])
    serial5 = os.path.join(tmp, "serial_div0.log")
    args5 = [
        qemu, "-name", "VimtuOS-r3drv-div0",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img5,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial5,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args5, stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log5 = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log5 = read_text(serial5)
            if re.search(r"\[EXT4SVC\] register ret=0 restarts=1 ok", log5):
                time.sleep(3.0)
                break
            if proc.poll() is not None:
                break
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try: proc.kill()
            except Exception: pass
    log5 = read_text(serial5)

    check("服务按 crash=2 主动崩溃（mode=2）",
          "[EXT4SVC] crash test deliberately faulting mode=2" in log5)
    m_de = re.search(r"\[SIG64\] fault pid=\d+ no=0 #DE ", log5)
    check("内核把它当**真硬件异常**处理（[SIG64] fault … no=0 #DE），不是 exit()",
          m_de is not None, m_de.group(0) if m_de else "缺")
    m_dfl = re.search(r"\[SIG64\] default action pid=\d+ sig=8 exit=\d+ fault=#DE", log5)
    check("默认动作 = 只杀该进程（sig=8 SIGFPE + fault=#DE），内核不 PANIC",
          m_dfl is not None, m_dfl.group(0) if m_dfl else "缺")
    m_die = re.search(r"\[R3DRV\] service died name=ext4svc[^\r\n]*lat_ms=(\d+)", log5)
    check("监督器在同一秒内发现（service died … reason=exit … lat_ms=N）",
          m_die is not None, m_die.group(0) if m_die else "缺")
    m_re = re.search(r"\[R3DRV\] service restart name=ext4svc attempt=1 pid=\d+ ok lat_ms=(\d+)", log5)
    check("**1 秒内拉起**（service restart … ok lat_ms<=1000）",
          m_re is not None and int(m_re.group(1)) <= 1000,
          ("lat_ms=%s" % m_re.group(1)) if m_re else "缺")
    if m_re is not None:
        check("实测延迟确实是被测出来的（lat_ms <= 1000 且 > 0）",
              0 < int(m_re.group(1)) <= 1000, "lat_ms=%s" % m_re.group(1))
    check("新实例重新注册、restarts=1，并且又解析了一遍 ext4（服务真的活了）",
          re.search(r"\[EXT4SVC\] register ret=0 restarts=1 ok", log5) is not None and
          len(re.findall(r"\[EXT4SVC\] file path=/hello\.txt", log5)) >= 1)
    check("整机存活：之后仍有心跳/serve 打点", re.search(r"\[R3DRV\] beat slot=0", log5) is not None)
    check("阶段 5 无 PANIC", "PANIC" not in log5)
    # ==================== 汇总 ====================
    passed = sum(1 for _n, c in checks if c)
    total = len(checks)
    print("\n=== 汇总：%d/%d ===" % (passed, total))
    for n, c in checks:
        if not c:
            print("  FAIL: %s" % n)
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("  保留临时目录：%s" % tmp)
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
