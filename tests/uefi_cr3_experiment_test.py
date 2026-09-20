#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/uefi_cr3_experiment_test.py - 批次 D：UEFI 运行期 CR3 实验（QEMU+OVMF / VMware EFI）

被测对象（kernel/proc64.cpp 末尾的 `#if PROC64_UEFI_CR3_EXPERIMENT` 段，默认**不编进**默认构建）：
  * 方案 A（就地挂载）：临时清 CR0.WP -> 往**固件活动 PML4** 的 PDPT[4] 挂自建 PD -> 恢复 WP
      -> 开 usermode64 的 WP-kludge + 覆盖 user64_available64 -> 试进 ring3；
  * 方案 B（自带 PML4）：新建 PML4/PDPT，复制固件的 512 项 + 0..4GB 的 PDPTE，PDPT[4] 留空
      -> 打 stage=cr3（**在 mov cr3 之前**）-> mov cr3 -> 活了就打 stage=enter -> 试进 ring3；
  * 两个方案都跑，最后一个结果行：`[PROC64] uefi exp result=A|B|none mode=isolated|shared`。
  实验由内核建的一个**延迟 5 秒**的任务执行（gui64_run 之前建）：它跑的时候 [GUI64] ready
  已经打出来了，所以即使 VMware EFI 下 mov cr3 触发复位，"桌面起来了"这条证据也已经留在串口里。

本脚本做什么：
  1) 用**实验内核**（默认 build64/kernel64_os_cr3exp.bin，由
        VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh
     构建后拷出来）造一个 UEFI 夹具盘（ESP + VimtuFS2 主分区），在 QEMU+OVMF 里启动：
       - 断言实验打点齐全（A/B 各自的 stage 行）；
       - 断言 [GUI64] ready（桌面照常起）；
       - 出现 result 行时解析 result/mode 并断言二者自洽（B -> isolated；A/none -> shared）；
       - 若日志停在 `B stage=cr3` 之后（复位/挂死），如实记为"B 触发复位"并检查没有 PANIC；
       - 禁止 PANIC / TRIPLE FAULT。
  2) 用同一块夹具盘在 **VMware EFI** 里跑一遍（firmware=efi 的独立 vmx + 独立串口文件，
     目录沿用 tests/uefi64_install_test.py 的 C:\\Users\\fanqi\\Desktop\\新建文件夹\\v64-uefi-test）：
     - 若 VMware 下 mov cr3 触发复位：串口停在 `B stage=cr3` 之后 -> 如实记录（不算脚本失败，
       但当 QEMU 下 B 成功而 VMware 下停在 cr3 时，结论必须写成"两条固件路径行为不同"）；
     - 若 VMware 下也出现 result 行：与 QEMU 的结果一起断言。
     没有 vmrun / 没有实验内核 / --no-vmware 时打印 SKIP 理由（不算失败）。

用法（必须用 Windows 原生 Python）：
    py -3 tests\\uefi_cr3_experiment_test.py                 # QEMU+OVMF + VMware EFI
    py -3 tests\\uefi_cr3_experiment_test.py --qemu-only     # 只跑 QEMU+OVMF
退出码：0 = 全过（含"如实失败并记录"）；1 = 断言失败；2 = 环境问题
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

import proc64_test as p64          # noqa: E402  （夹具/OVMF/关键词法复用）

KERNEL_EXP = os.path.join(ROOT, "build64", "kernel64_os_cr3exp.bin")
FIXTURE_EXP = os.path.join(ROOT, "build64", "cr3exp_uefi.img")
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
VMDIR = r"C:\Users\fanqi\Desktop\新建文件夹\v64-uefi-test"
VMX = os.path.join(VMDIR, "vimtu64-cr3exp.vmx")
VMIMG = os.path.join(VMDIR, "cr3exp.img")
VMVMDK = os.path.join(VMDIR, "cr3exp.vmdk")
VMSERIAL = os.path.join(VMDIR, "serial-cr3exp.log")


# ---------------------------------------------------------------------------
# 夹具：ESP（实验内核）+ VimtuFS2 主分区（与 proc64_test.prepare_uefi_fixture 同一套布局）
# ---------------------------------------------------------------------------
def make_esp(stage):
    esp = os.path.join(ROOT, "build64", "esp_cr3exp_%s.img" % stage)
    if os.path.exists(esp):
        os.remove(esp)
    args = [sys.executable, os.path.join(ROOT, "tools", "make_esp.py"), esp,
            os.path.join(ROOT, "build64", "BOOTX64.EFI"), KERNEL_EXP,
            os.path.join(ROOT, "build64", "system.img"),
            os.path.join(ROOT, "build64", "UEFI64.BIN")]
    r = subprocess.run(args, capture_output=True, timeout=300)
    if r.returncode != 0 or not os.path.exists(esp):
        sys.stderr.write(r.stdout.decode("utf-8", "replace")[-400:] + r.stderr.decode("utf-8", "replace")[-400:])
        return None
    return esp


def make_fixture():
    esp = make_esp("qemu")
    if not esp:
        return None
    with open(esp, "rb") as f:
        esp_bytes = f.read()
    # make_esp.py 现在生成 48MB 的**真 FAT32** ESP：分区大小按镜像实际字节数写
    # （p64.ESP_SECTORS 是旧的 FAT16 常量，写小了固件只看到分区那一截）
    esp_sectors = (len(esp_bytes) + p64.SECTOR - 1) // p64.SECTOR
    total = p64.ESP_LBA + esp_sectors + 8192
    total = ((total + 2047) // 2048) * 2048
    buf = bytearray(total * p64.SECTOR)
    buf[p64.ESP_LBA * p64.SECTOR:p64.ESP_LBA * p64.SECTOR + len(esp_bytes)] = esp_bytes
    main_lba = p64.ESP_LBA + esp_sectors
    main_sectors = total - main_lba - 1
    if main_sectors < 4096:
        return None
    p64._vimtufs2_format(buf, main_lba, main_sectors)
    buf[446:462] = p64._mbr_entry(True, 0xEF, p64.ESP_LBA, esp_sectors)
    buf[462:478] = p64._mbr_entry(False, 0x07, main_lba, main_sectors)
    buf[510], buf[511] = 0x55, 0xAA
    with open(FIXTURE_EXP, "wb") as f:
        f.write(buf)
    return FIXTURE_EXP


# ---------------------------------------------------------------------------
# 串口日志工具
# ---------------------------------------------------------------------------
def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_marker(path, needles, timeout, proc=None):
    """等 needles 里任意一个出现（needles 可以是 str 或 list）；返回 (log, hit_needle|None)。"""
    if isinstance(needles, str):
        needles = [needles]
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = read_text(path)
        for n in needles:
            if n in s:
                return s, n
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(1.0)
    return read_text(path), None


# ---------------------------------------------------------------------------
# 结果判定（QEMU 与 VMware 共用）
# ---------------------------------------------------------------------------
def analyze(log):
    """把一份串口日志里的实验事实抽出来（不做断言，只给结论）。"""
    out = {
        "gui_ready": "[GUI64] ready" in log,
        "init_shared": "[PROC64] init mode=shared" in log,
        "task_started": "[PROC64] uefi exp task started" in log,
        "begin": "[PROC64] uefi exp begin" in log,
        "a_map_ok": re.search(r"\[PROC64\] uefi exp A stage=map err=0000000000000000", log) is not None,
        "a_enter": "[PROC64] uefi exp A stage=enter" in log,
        "a_ok": "[PROC64] uefi exp A stage=ok" in log,
        "a_fail": re.search(r"\[PROC64\] uefi exp A stage=fail err=([0-9A-F]{16})", log),
        "b_build_ok": re.search(r"\[PROC64\] uefi exp B stage=build err=0000000000000000", log) is not None,
        "b_cr3": "[PROC64] uefi exp B stage=cr3" in log,
        "b_enter": "[PROC64] uefi exp B stage=enter" in log,
        "b_ok": "[PROC64] uefi exp B stage=ok" in log,
        "b_fail": re.search(r"\[PROC64\] uefi exp B stage=fail err=([0-9A-F]{16})", log),
        "result": re.search(r"\[PROC64\] uefi exp result=(\w+) mode=(\w+)", log),
        "isolation_on": "[PROC64] cr3 isolation ON" in log,
        "panic": ("PANIC" in log) or ("TRIPLE FAULT" in log),
        "reset_after_cr3": False,
    }
    if out["b_cr3"] and not out["b_enter"] and not out["panic"]:
        out["reset_after_cr3"] = True                 # 停在 mov cr3 之后：复位/挂死（如实记录）
    if out["result"]:
        out["result_val"] = out["result"].group(1)
        out["mode_val"] = out["result"].group(2)
    else:
        out["result_val"] = None
        out["mode_val"] = None
    return out


# ---------------------------------------------------------------------------
# QEMU + OVMF
# ---------------------------------------------------------------------------
def run_qemu(qemu, img, tag, logdir, timeout):
    ovmf = p64.find_ovmf()
    if not ovmf:
        return None, "找不到 OVMF（%s）" % ", ".join(p64.OVMF_CANDIDATES)
    serial = os.path.join(logdir, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    args = [qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % p64.q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % p64.q(serial),
            "-no-reboot",
            "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % p64.q(ovmf),
            "-drive", "if=pflash,format=raw,unit=1,file=%s" % p64.q(p64._ovmf_vars())]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log, hit = wait_marker(serial, ["[PROC64] uefi exp result=", "[PROC64] uefi exp B stage=cr3"],
                           timeout, proc)
    # 结果行/失败行之后再给一点时间把后续（GUI/isolation ON 行）刷完
    time.sleep(3.0)
    log = read_text(serial)
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    return log, None


# ---------------------------------------------------------------------------
# VMware EFI
# ---------------------------------------------------------------------------
def write_vmdk(path, flat_name, sectors, cid):
    txt = """# Disk DescriptorFile
version=1
encoding="UTF-8"
CID=%08x
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW %d FLAT "%s" 0

# The Disk Data Base
#DDB

ddb.virtualHWVersion = "7"
ddb.geometry.cylinders = "%d"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
ddb.adapterType = "ide"
""" % (cid, sectors, flat_name, max(1, sectors // (16 * 63)))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)


def write_vmx(path, title, serial_log):
    txt = """#!/usr/bin/vmware
.encoding = "UTF-8"
config.version = "8"
virtualHW.version = "7"
displayName = "%s"
guestOS = "other-64"
msg.autoAnswer = "TRUE"
numvcpus = "1"
memsize = "512"
svga.vramSize = "134217728"
ide0:0.present = "TRUE"
ide0:0.deviceType = "disk"
ide0:0.fileName = "cr3exp.vmdk"
ide0:0.startConnected = "TRUE"
ide0:1.present = "FALSE"
ide1:0.present = "FALSE"
floppy0.present = "FALSE"
ethernet0.present = "FALSE"
usb.present = "FALSE"
sound.present = "FALSE"
vmci0.present = "FALSE"
tools.syncTime = "FALSE"
tools.install.state = "none"
tools.upgrade.policy = "manual"
mks.enable3d = "FALSE"
isolation.tools.hgfs.disable = "TRUE"
firmware = "efi"
uefi.secureBoot = "FALSE"
bios.bootDelay = "2000"
serial0.present = "TRUE"
serial0.fileType = "file"
serial0.fileName = "%s"
serial0.tryNoRxLoss = "FALSE"
serial0.startConnected = "TRUE"
""" % (title, serial_log.replace("\\", "\\\\"))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)


def vmrun(*args, timeout=240):
    return subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)


def vm_stop_all():
    r = vmrun("list")
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        if line.strip().endswith(".vmx"):
            vmrun("stop", line.strip(), "hard")


def run_vmware(img, timeout):
    if not os.path.exists(VMRUN):
        return None, "找不到 vmrun（%s）" % VMRUN
    if not os.path.isdir(VMDIR):
        return None, "找不到 VMware 测试目录（%s）" % VMDIR
    shutil.copyfile(img, VMIMG)
    sectors = os.path.getsize(VMIMG) // 512
    write_vmdk(VMVMDK, "cr3exp.img", sectors, 0x33333333)
    if os.path.exists(VMSERIAL):
        os.remove(VMSERIAL)
    write_vmx(VMX, "VimtuOS 64 位 UEFI CR3 实验", VMSERIAL)
    vm_stop_all()
    started = False
    for _ in range(6):
        r = vmrun("start", VMX, "nogui")
        if r.returncode == 0:
            started = True
            break
        time.sleep(4)
    if not started:
        return None, "vmrun start 失败"
    try:
        log, hit = wait_marker(VMSERIAL, ["[PROC64] uefi exp result=", "[PROC64] uefi exp B stage=cr3"],
                               timeout)
        time.sleep(3.0)
        log = read_text(VMSERIAL)
    finally:
        vm_stop_all()
    return log, None


def main():
    global KERNEL_EXP
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--kernel", default=KERNEL_EXP, help="实验内核（默认 build64/kernel64_os_cr3exp.bin）")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--qemu-only", action="store_true")
    ap.add_argument("--vmware-only", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--build-exp-kernel", action="store_true",
                    help="缺实验内核时自动做一次实验构建（会覆盖 build64/，记得再跑一次 bash build64.sh 恢复默认）")
    args = ap.parse_args()
    KERNEL_EXP = args.kernel          # global 声明在函数首行（模块级默认值见文件头）

    ok = True

    # ★ 可选：自动做一次实验构建（默认不做 —— 它会覆盖 build64/ 里的默认内核/镜像，
    #   构建完请再跑一次 bash build64.sh 恢复默认产物；见 docs/UEFI地址空间实验报告.md）。
    if args.build_exp_kernel and not os.path.exists(KERNEL_EXP):
        bash = r"C:\msys64\usr\bin\bash.exe"
        if os.path.exists(bash):
            print("=== 实验构建：VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh ===")
            r = subprocess.run([bash, "-lc",
                                "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && "
                                "VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh && "
                                "cp build64/kernel64_os.bin build64/kernel64_os_cr3exp.bin"],
                               capture_output=True, timeout=1800)
            print("    构建退出码 = %d" % r.returncode)
        else:
            print("--- 实验构建：SKIP（找不到 %s）---" % bash)
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    qemu = p64.find_qemu(args.qemu)
    exp_log = None
    vm_log = None

    if not args.vmware_only:
        if not os.path.exists(KERNEL_EXP):
            print("--- QEMU+OVMF：SKIP（缺少实验内核 %s）---" % KERNEL_EXP)
            print("    构建：VIMTU_EXTRA_CXXFLAGS=-DPROC64_UEFI_CR3_EXPERIMENT=1 bash build64.sh，")
            print("    然后：cp build64/kernel64_os.bin build64/kernel64_os_cr3exp.bin")
            if not qemu:
                return 2
        elif not qemu:
            print("--- QEMU+OVMF：SKIP（找不到 qemu-system-x86_64）---")
        else:
            img = make_fixture()
            if not img:
                print("--- QEMU+OVMF：SKIP（造 ESP 夹具失败：需要 build64/BOOTX64.EFI / UEFI64.BIN）---")
            else:
                print("=== QEMU+OVMF：实验内核 %s ===" % os.path.basename(KERNEL_EXP))
                tmp = tempfile.mkdtemp(prefix="vimtu64_cr3exp_")
                exp_log, err = run_qemu(qemu, img, "cr3exp", tmp, args.timeout)
                if err:
                    print("--- QEMU+OVMF：SKIP（%s）---" % err)
                    exp_log = None

    if exp_log is not None:
        a = analyze(exp_log)
        print("--- QEMU+OVMF 判定 ---")
        check("桌面照常起（[GUI64] ready）", a["gui_ready"])
        check("实验构建启动时仍然是 mode=shared（实验是**运行期**打开的）", a["init_shared"])
        check("实验任务已建（[PROC64] uefi exp task started）", a["task_started"])
        check("实验开始（[PROC64] uefi exp begin）", a["begin"])
        check("方案 A：stage=map 打点（err 行齐全）",
              re.search(r"\[PROC64\] uefi exp A stage=map err=[0-9A-F]{16}", exp_log) is not None)
        check("方案 A：stage=enter 打点", a["a_enter"])
        check("方案 A：结果行（stage=ok 或 stage=fail）", a["a_ok"] or a["a_fail"] is not None,
              "ok" if a["a_ok"] else (("fail err=%s" % a["a_fail"].group(1)) if a["a_fail"] else "（缺）"))
        check("方案 B：stage=build 打点", a["b_build_ok"])
        check("方案 B：stage=cr3 打点（mov cr3 之前的最后一行）", a["b_cr3"])
        check("方案 B：mov cr3 之后还活着（stage=enter）→ 运行期 mov cr3 可用", a["b_enter"],
              "" if a["b_enter"] else "（串口停在 stage=cr3 之后 = 复位/挂死）")
        check("方案 B：结果行（stage=ok 或 stage=fail）", a["b_ok"] or a["b_fail"] is not None,
              "ok" if a["b_ok"] else (("fail err=%s" % a["b_fail"].group(1)) if a["b_fail"] else "（缺）"))
        check("结果行 [PROC64] uefi exp result=A|B|none mode=isolated|shared", a["result"] is not None,
              a["result"].group(0) if a["result"] else "（缺；可能是 B 触发复位）")
        if a["result"]:
            if a["result_val"] == "B":
                check("result=B -> mode=isolated 且打印 cr3 isolation ON", a["mode_val"] == "isolated" and a["isolation_on"])
            else:
                check("result=%s -> mode=shared（如实降级，不假称隔离）" % a["result_val"],
                      a["mode_val"] == "shared")
        check("不得出现 PANIC / TRIPLE FAULT", not a["panic"])
        if args.keep:
            print("[cr3exp] QEMU 串口：%s" % os.path.join(tempfile.gettempdir(), ".."))

    if not args.qemu_only:
        print("=== VMware EFI：实验内核同一块夹具盘 ===")
        img = FIXTURE_EXP if os.path.exists(FIXTURE_EXP) else None
        if exp_log is None and img is None:
            img = make_fixture()
        if img is None:
            print("--- VMware EFI：SKIP（没有可挂的夹具盘）---")
        else:
            vm_log, err = run_vmware(img, args.timeout)
            if err:
                print("--- VMware EFI：SKIP（%s）---" % err)
                vm_log = None

    if vm_log is not None:
        b = analyze(vm_log)
        print("--- VMware EFI 判定 ---")
        check("VMware EFI：进了长模式内核（[LM64] ENTERED LONG MODE）",
              "[LM64] ENTERED LONG MODE" in vm_log)
        check("VMware EFI：桌面照常起（[GUI64] ready）", b["gui_ready"])
        check("VMware EFI：实验打点出现（A stage=map）",
              re.search(r"\[PROC64\] uefi exp A stage=map err=[0-9A-F]{16}", vm_log) is not None)
        check("VMware EFI：方案 B：stage=cr3 打点", b["b_cr3"])
        if b["reset_after_cr3"]:
            print("  [记录] ★ VMware EFI 下 mov cr3 之后串口不再前进（停在 stage=cr3）= **复位/挂死现象**")
            check("VMware EFI：B 触发复位/挂死（如实记录，脚本不算失败）", True)
            check("VMware EFI：复位现象里没有 PANIC 行（复位前没有异常处理路径）", not b["panic"])
        else:
            check("VMware EFI：mov cr3 之后还活着（stage=enter）", b["b_enter"])
            check("VMware EFI：结果行出现", b["result"] is not None,
                  b["result"].group(0) if b["result"] else "（缺）")
            if b["result"]:
                if b["result_val"] == "B":
                    check("VMware EFI：result=B -> mode=isolated", b["mode_val"] == "isolated")
                else:
                    check("VMware EFI：result=%s -> mode=shared" % b["result_val"], b["mode_val"] == "shared")
        check("VMware EFI：不得出现 TRIPLE FAULT", "TRIPLE FAULT" not in vm_log)
        # 两条固件路径的差异结论（只打印，不判失败 —— 这是"如实记录"的一部分）
        if exp_log is not None:
            a = analyze(exp_log)
            print("  [对照] QEMU+OVMF : B stage=enter=%s result=%s mode=%s" %
                  (a["b_enter"], a["result_val"], a["mode_val"]))
            print("  [对照] VMware EFI: B stage=enter=%s result=%s mode=%s 复位=%s" %
                  (b["b_enter"], b["result_val"], b["mode_val"], b["reset_after_cr3"]))

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
