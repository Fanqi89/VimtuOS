#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ext4rpc64_test.py - ★ v0.9.40：内核 -> ring3 服务的**反向 RPC**（ext4 只读挂载的底座）

要证明的东西（这一条通了，"文件管理器挂载 ext4"才有地基）：
  ① **握手**：内核按协议找到活着的 ext4 服务、取到共享对象帧、看到服务发布的 req.magic；
  ② **LIST**：内核通过 RPC 让服务列 "/"，拿回 3 条目录项（hello.txt / big.bin / sub）；
  ③ **STAT**：/big.bin 的 size=100000、type=1（普通文件）；
  ④ **READ**：内核把 /big.bin 的 100000 字节全读出来并算 CRC32，
     与**宿主独立算的** crc32 **逐位相等**（宿主用同一套确定性图案 (i*31+7)&0xFF 生成期望字节，
     不解析镜像、不信任 guest 自报）；
  ⑤ **不卡死**：每条 RPC 的 ms 都 ≤ 150（内核有界等待），并且 timeout 探针（上限压到 0）
     必须立刻返回 -ETIMEDOUT —— 这就是"GUI 不会被服务拖死"的证据；
  ⑥ 之后系统仍然活着（服务还在心跳、没有 PANIC）。

怎么切场景：镜像里的 /etc/r3drv.conf 是纯 ASCII，测试对镜像做**等长字节替换**（rpc=0 -> rpc=1）。

用法：py -3 tests/ext4rpc64_test.py [--qemu PATH] [--timeout 300] [--keep]
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
MKFS = os.path.join(ROOT, "tools", "mkfs_ext4_win.py")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files\qemu\qemu-system-x86_64",
    "qemu-system-x86_64",
]

FX_BLOCKS = 16384
BIG_SIZE = 100000
RPC_LIMIT_MS = 150          # 必须与 kernel/ext4vol64.h 的 EXT4VOL_RPC_MS 一致


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
    data = img_bytes
    for old, new in pairs:
        assert len(old) == len(new), (old, new)
        if data.count(old) == 0:
            raise SystemExit("镜像里找不到 %r（先跑全量构建，conf 里要有 rpc=0）" % old)
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
    for need in (SYSDISK, EXT4SVC, MKFS):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_ext4rpc_")
    fx = os.path.join(tmp, "ext4fx.img")
    print("=== 0) 生成 ext4 夹具（%d 块，自校验）===" % FX_BLOCKS)
    rc = subprocess.call([sys.executable, MKFS, "--out", fx, "--blocks", str(FX_BLOCKS)],
                         stdout=subprocess.DEVNULL)
    if rc != 0 or not os.path.exists(fx):
        sys.stderr.write("夹具生成失败 rc=%s\n" % rc)
        return 2

    # 宿主侧**独立**算出 /big.bin 的期望字节与 CRC32（不解析镜像、不看 guest 自报）
    want = bytes(((i * 31 + 7) & 0xFF) for i in range(BIG_SIZE))
    want_crc = zlib.crc32(want) & 0xFFFFFFFF
    print("    /big.bin 期望：%d B，crc32=0x%08X（宿主用 (i*31+7)&0xFF 生成）" % (BIG_SIZE, want_crc))

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== 1) 启动并让内核跑 RPC 自检（rpc=1）===")
    data = patch_conf(open(SYSDISK, "rb").read(), [(b"rpc=0", b"rpc=1")])
    img = os.path.join(tmp, "r3drv_rpc.img")
    with open(img, "wb") as f:
        f.write(data)

    serial = os.path.join(tmp, "serial_rpc.log")
    if os.path.exists(serial):
        os.remove(serial)
    args_qemu = [
        qemu, "-name", "VimtuOS-ext4rpc",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args_qemu, stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(tmp, "qemu_stderr.txt"), "ab"))
    log = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log = read_text(serial)
            if re.search(r"\[EXT4VOL\] selftest timeout-probe", log):
                break
            if proc.poll() is not None:
                break
        # 再等一会儿，让"之后系统仍活着"的断言有素材（心跳/serve 打点）
        time.sleep(3.0)
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    log = read_text(serial)

    print("=== 2) 握手与三条 RPC ===")
    m_bind = re.search(r"\[EXT4VOL\] bind slot=(\d+) shm=(\d+) ok", log)
    check("内核握手成功（[EXT4VOL] bind … ok，服务已发布 req.magic）",
          m_bind is not None, m_bind.group(0) if m_bind else "缺")

    m_list = re.search(r'\[EXT4VOL\] selftest list="/" count=(\d+) names=([^\s]+) ok', log)
    check("LIST \"/\" 返回 3 条（hello.txt,big.bin,sub）",
          m_list is not None and m_list.group(1) == "3"
          and all(n in m_list.group(2) for n in ("hello.txt", "big.bin", "sub")),
          m_list.group(0) if m_list else "缺")

    m_stat = re.search(r'\[EXT4VOL\] selftest stat="/big\.bin" size=(\d+) type=(\d+) ok', log)
    check("STAT /big.bin = 100000 B，type=1（普通文件）",
          m_stat is not None and m_stat.group(1) == str(BIG_SIZE) and m_stat.group(2) == "1",
          m_stat.group(0) if m_stat else "缺")

    m_read = re.search(r'\[EXT4VOL\] selftest read="/big\.bin" bytes=(\d+) crc=0x([0-9a-fA-F]+) ok', log)
    check("READ /big.bin 读满 100000 B 且内核判定 ok",
          m_read is not None and m_read.group(1) == str(BIG_SIZE),
          m_read.group(0) if m_read else "缺")
    if m_read:
        got_crc = int(m_read.group(2), 16) & 0xFFFFFFFF
        check("内核算出的 CRC32 与宿主独立算的**逐位相等**",
              got_crc == want_crc,
              "kernel=0x%08x host=0x%08x" % (got_crc, want_crc))
    else:
        check("内核算出的 CRC32 与宿主独立算的**逐位相等**", False, "没有 read 行")

    print("=== 3) 不卡死：每条 RPC 有界，超时探针必失败 ===")
    ms_all = [int(m) for m in re.findall(r"\[EXT4VOL\] rpc op=\w+ path=[^\s]* result=\d+ ms=(\d+)", log)]
    check("每条 RPC 的耗时都 ≤ %d ms（实测 %s）" % (RPC_LIMIT_MS, ms_all[:8]),
          len(ms_all) > 0 and max(ms_all) <= RPC_LIMIT_MS,
          "max=%s n=%d" % (max(ms_all) if ms_all else "n/a", len(ms_all)))
    check("超时探针（上限压到 0）确实立刻超时（selftest timeout-probe=ok）",
          re.search(r"\[EXT4VOL\] selftest timeout-probe=ok", log) is not None)

    print("=== 4) 服务侧也留下了成对证据 ===")
    check("服务处理过 list 请求（[EXT4SVC] rpc op=list path=/ result=3 ok）",
          re.search(r"\[EXT4SVC\] rpc op=list path=/ result=3 ok", log) is not None,
          (re.search(r"\[EXT4SVC\] rpc op=list[^\r\n]*", log).group(0)
           if re.search(r"\[EXT4SVC\] rpc op=list", log) else "缺"))
    check("服务处理过 read 请求（[EXT4SVC] rpc op=read path=/big.bin … ok）",
          re.search(r"\[EXT4SVC\] rpc op=read path=/big\.bin result=\d+ ok", log) is not None,
          (re.search(r"\[EXT4SVC\] rpc op=read[^\r\n]*", log).group(0)
           if re.search(r"\[EXT4SVC\] rpc op=read", log) else "缺"))

    print("=== 5) 系统仍然活着 ===")
    check("之后仍有服务心跳（[R3DRV] beat slot=0）",
          re.search(r"\[R3DRV\] beat slot=0", log) is not None)
    check("之后仍有 ext4svc 常驻打点（[EXT4SVC] serve round=）",
          re.search(r"\[EXT4SVC\] serve round=", log) is not None)
    check("没有 PANIC", "[PANIC64]" not in log)

    npass = sum(1 for _, c in checks if c)
    print("\n%d/%d PASS" % (npass, len(checks)))
    if npass != len(checks):
        print("FAILED: %s" % "; ".join(n for n, c in checks if not c))
        print("日志保留在 %s" % tmp)
        print("FAIL")
        return 1
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS：内核通过**反向 RPC**让 ring3 服务列出并读出 ext4 的文件，CRC 与宿主逐位相等，且有界不卡死")
    return 0


if __name__ == "__main__":
    sys.exit(main())
