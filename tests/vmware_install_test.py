#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vmware_install_test.py - 在 VMware 上跑完真实安装流程并做字节级验收。

流程（和人在 VMware 里点鼠标一样，只是按键从串口 COM2 送进去）：
  1) 重建 16MB 空目标盘 + 两份 vmx（安装 VM / 已安装盘启动 VM）
  2) 启动安装 VM（BIOS 固件、IDE0:0=安装介质、IDE0:1=目标盘）
  3) 等向导就绪 -> 通过 \\\\.\\pipe\\v64keys 送按键：回车×4、n（新建）、回车（安装系统）
  4) 串口断言：[PART] 新建、[INSTALL] 开始/完成、[SETUP] 安装完成、[SETUP] 自动重启
  5) 解析目标盘：MBR 55AA + P1(0xEF,活动,9+8000) + P2(0x07,8009..盘尾)、
     LBA1..8 = loader64.bin、LBA9.. = kernel64_os.bin（逐字节）
  6) 单独启动目标盘：断言走 [OS] 路径（不是安装程序）
"""
import argparse
import os
import struct
import subprocess
import sys
import socket
import time

ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
OUTDIR = r"C:\Users\fanqi\Desktop\新建文件夹\v64-install-test"
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
VMX_INSTALL = os.path.join(OUTDIR, "vimtu64-install-test.vmx")
VMX_BOOT = os.path.join(OUTDIR, "vimtu64-installed-boot.vmx")

# Windows 控制台默认 GBK：本脚本会打印 vmrun/固件的非 ASCII 输出（含替换符 U+FFFD），
# 不强制 UTF-8 会以 UnicodeEncodeError 假失败（与测试逻辑无关，纯输出编码问题）。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass
SERIAL_INSTALL = os.path.join(OUTDIR, "serial-install.log")
SERIAL_BOOT = os.path.join(OUTDIR, "serial-installed-boot.log")
TARGET = os.path.join(OUTDIR, "target.img")
OS_KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")
HERE = os.path.dirname(os.path.abspath(__file__))
SCRATCH = os.environ.get("PI_SCRATCH_DIR", HERE)

LOADER = os.path.join(ROOT, "build64", "loader64.bin")
KEY_TCP_PORT = 4557            # COM2 按键通道（VMware network 模式串口，服务端在 VMware 侧）
INS_KERNEL = os.path.join(ROOT, "build64", "kernel64.bin")
TARGET_SECTORS = 32768


def vmrun(*args, timeout=180):
    r = subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


def stop_all():
    code, out = vmrun("list")
    for line in out.splitlines():
        line = line.strip()
        if line.lower().endswith(".vmx"):
            vmrun("stop", line, "hard")
    # 等 VMware 真正把 VM 停掉：否则磁盘锁没释放，紧接着 start 会被取消
    for _ in range(40):
        code, out = vmrun("list")
        if "Total running VMs: 0" in out:
            break
        time.sleep(1)


def start_vm(vmx, tries=6):
    out = ""
    for i in range(tries):
        code, out = vmrun("start", vmx, "nogui")
        if code == 0:
            return True, out
        print("   [start 重试 %d/%d] %s" % (i + 1, tries, out.strip()[:140]))
        time.sleep(6)
    return False, out




def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_for(path, needle, seconds, what):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if needle in read_text(path):
            print("   [ok] %s  (%s)" % (what, needle))
            return True
        time.sleep(0.5)
    print("   [!!] 等超时：%s（没看到 %r）" % (what, needle))
    return False




def check_mbr(path):
    with open(path, "rb") as f:
        sec = f.read(512)
    if len(sec) < 512 or sec[510] != 0x55 or sec[511] != 0xAA:
        return None, None
    out = []
    for i in range(2):
        e = sec[446 + i * 16: 446 + i * 16 + 16]
        if e[4] == 0:
            out.append(None)
            continue
        out.append({"boot": e[0] == 0x80, "type": e[4],
                    "start": struct.unpack_from("<I", e, 8)[0],
                    "sectors": struct.unpack_from("<I", e, 12)[0]})
    return out[0], out[1]


def open_key_channel(port=KEY_TCP_PORT, seconds=90):
    """连上 VMware 的 TCP 串口（COM2），返回写侧 socket；用于送按键。"""
    t0 = time.time()
    last = None
    while time.time() - t0 < seconds:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError as e:
            last = e
            time.sleep(0.5)
    print("   [!!] 连不上 COM2 通道 127.0.0.1:%d：%s" % (port, last))
    return None


def send_keys(sock, keys, dwell=1.5):
    for k in keys:
        sock.sendall(bytes([k]))
        print("   [key] 0x%02X" % k)
        time.sleep(dwell)


def read_at(path, lba, nbytes):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        return f.read(nbytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-target", action="store_true")
    args = ap.parse_args()

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    print("=== 0) 停掉旧 VM，准备磁盘与 vmx ===")
    stop_all()
    extra = (["--keep-target"] if args.keep_target else [])
    if os.environ.get("VIMTU_VM_ISO") == "1":
        extra.append("--iso")
    r = subprocess.run([sys.executable, os.path.join(HERE, "vmware_make_vm.py")] + extra,
                       capture_output=True, timeout=300)
    print(r.stdout.decode("utf-8", "replace").strip())
    if r.returncode != 0:
        print("准备失败：%s" % r.stderr.decode("utf-8", "replace"))
        return 2
    for f in (TARGET, LOADER, OS_KERNEL, INS_KERNEL):
        if not os.path.exists(f):
            print("缺少 %s" % f)
            return 2

    print("=== 1) 启动 VMware 安装 VM（BIOS 固件）===")
    ok_start, out = start_vm(VMX_INSTALL)
    print("   starVM ok=%s %s" % (ok_start, out.strip()[:200]))

    print("=== 2) 等向导就绪（串口出现磁盘枚举完成）===")
    if not wait_for(SERIAL_INSTALL, "磁盘枚举完成", 180, "向导就绪"):
        print("--- 串口尾部 ---")
        for l in read_text(SERIAL_INSTALL).splitlines()[-15:]:
            print("   | " + l[:150])
    print("=== 3) 通过 COM2（TCP 串口）送按键走完向导 ===")
    sock = open_key_channel()
    if sock is None:
        return 1
    try:
        # 回车×4：语言 -> 现在安装 -> 许可 -> 安装类型 -> 磁盘与分区
        send_keys(sock, [0x0D, 0x0D, 0x0D, 0x0D], dwell=1.2)
        # 'n' = 新建（写引导分区 + 主分区）
        send_keys(sock, [ord('n')], dwell=2.0)
        # 回车 = 安装系统
        send_keys(sock, [0x0D], dwell=1.0)
    finally:
        sock.close()
    print("=== 4) 等安装完成（真实拷贝 8073 扇区）===")
    wait_for(SERIAL_INSTALL, "[INSTALL] 完成：已写", 120, "安装完成")
    wait_for(SERIAL_INSTALL, "[SETUP] 自动重启", 60, "自动重启")
    time.sleep(2)
    log = read_text(SERIAL_INSTALL)

    print("=== 5) 串口断言 ===")
    check("向导在 VMware 里跑起来（安装程序启动）", "安装程序启动" in log)
    check("磁盘枚举看到 VMware 两块盘", "VMware Virtual IDE Hard Drive" in log)
    check("串口按键通道就绪", "串口按键通道 COM2 就绪" in log)
    check("按键真的进了向导（com2key）", "com2key=0x000000000000000D" in log)
    check("新建分区：写引导分区 + 主分区", "[PART] 新建分区表 OK" in log)
    check("安装开始：找到介质载荷", ("[INSTALL] 开始安装" in log) or ("[INSTALL] 介质源 kind=2" in log))
    check("安装完成：写出扇区数", "[INSTALL] 完成：已写" in log)
    check("界面侧确认 100%", "[SETUP] 安装完成 100%" in log)
    check("完成后自动重启", "[SETUP] 自动重启" in log)
    tail = [l for l in log.splitlines() if l.strip()][-12:]
    print("--- 串口尾部 ---")
    for l in tail:
        print("   | " + l[:150])

    print("=== 6) 停 VM，解析目标盘字节 ===")
    stop_all()
    time.sleep(2)
    p1, p2 = check_mbr(TARGET)
    check("MBR 签名 55AA 且分区1 存在", p1 is not None, "%s" % (p1,))
    if p1:
        check("分区1 = 引导分区 0xEF 且活动", p1["type"] == 0xEF and p1["boot"],
              "type=0x%02X boot=%s" % (p1["type"], p1["boot"]))
        check("分区1 = LBA 9 + 8000 扇区", p1["start"] == 9 and p1["sectors"] == 8000,
              "start=%d sectors=%d" % (p1["start"], p1["sectors"]))
    check("分区2 = 主分区 0x07", p2 is not None and p2["type"] == 0x07, "%s" % (p2,))
    if p2:
        check("分区2 起点 8009 覆盖到盘尾",
              p2["start"] == 8009 and p2["sectors"] == TARGET_SECTORS - 8009,
              "start=%d sectors=%d" % (p2["start"], p2["sectors"]))

    loader_ref = open(LOADER, "rb").read()
    os_ref = open(OS_KERNEL, "rb").read()
    ins_ref = open(INS_KERNEL, "rb").read()
    check("目标盘 LBA1..8 = loader64.bin（逐字节）",
          read_at(TARGET, 1, len(loader_ref)) == loader_ref, "长度 %d" % len(loader_ref))
    kern = read_at(TARGET, 9, len(os_ref))
    check("目标盘 LBA9.. = kernel64_os.bin（逐字节）", kern == os_ref, "长度 %d" % len(os_ref))
    check("目标盘内核不是安装程序内核", kern != ins_ref[:len(kern)])

    print("=== 7) 单独启动装好的硬盘（VMware 里验证真能开机）===")
    ok_boot, out = start_vm(VMX_BOOT)
    print("   startVM ok=%s %s" % (ok_boot, out.strip()[:200]))
    wait_for(SERIAL_BOOT, "[OS] ready (idle)", 120, "装好的系统就绪")
    time.sleep(1)
    boot = read_text(SERIAL_BOOT)
    stop_all()
    check("装好的盘进入长模式", "[LM64] ENTERED LONG MODE" in boot)
    check("走的是系统启动路径", "[OS] booted from installed disk" in boot)
    check("系统就绪", "[OS] ready (idle)" in boot)
    check("没有再进安装程序", "[SETUP]" not in boot and "entering setup wizard" not in boot)
    print("--- 装好的系统串口尾部 ---")
    for l in [x for x in boot.splitlines() if x.strip()][-8:]:
        print("   | " + l[:150])

    print("=== VMware RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
