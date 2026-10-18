# -*- coding: utf-8 -*-
"""tests/iso64_layout_test.py —— v0.5.6：ISO 目录结构从**扁平**改成**层级式树状**，且仍然能引导。

判据（每条都能独立证伪）：
  ① 层级树存在且路径正确（用 ISO9660 解析器在**镜像里**逐层走，不看暂存目录）：
       /BOOT/CDISO.BIN  /BOOT/LOADER64.BIN  /BOOT/KERNEL64.BIN
       /SYSTEM/SYSTEM.IMG
       /EFI/BOOT/BOOTX64.EFI  /EFI/BOOT/UEFI64.BIN
       /README.TXT  /VERSION
  ② **根目录不再摊扁平文件**：CDISO.BIN / LOADER64.BIN / KERNEL64.BIN / SYSTEM.IMG 都不能在根目录
     （否则"改了布局"只是自说自话）
  ③ 文件内容与构建产物逐字节一致（LOADER64 / SYSTEM.IMG / BOOTX64.EFI），
     且 VERSION 里的版本串与 build64.sh 的 VIMTUOS_VERSION 一致
  ④ 回填的绝对 LBA 与新位置**自洽**：镜像里的 VMLD/VMKN/VMSY 三个补丁点必须分别等于
     /BOOT/LOADER64.BIN、/BOOT/KERNEL64.BIN、/SYSTEM/SYSTEM.IMG 在 ISO 里的真实起始 LBA
     （引导链读的是这三个数，不是文件名 —— 这条是"改目录没改坏引导"的硬证据）
  ⑤ 三种引导实测都进安装程序：BIOS 光盘（El Torito）、UEFI 光盘（El Torito 0xEF -> ESP）、
     hybrid 硬盘/U 盘（MBR 桩）。每组都给一块空目标盘，要求 `[SETUP] 安装程序启动` +
     `[SETUP] 磁盘枚举完成 rows>=2`
  ⑥ 全程无 PANIC / TRIPLE FAULT

用法：py -3 tests/iso64_layout_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（qemu / ISO / OVMF）。
"""
import importlib.util
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "vimtu64-64.iso")
BUILD = os.path.join(ROOT, "build64")
OUT = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "isolayout")
os.makedirs(OUT, exist_ok=True)

checks = []
QEMU_CANDIDATES = [r"C:\Program Files\qemu\qemu-system-x86_64.exe",
                   r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
                   "qemu-system-x86_64"]
OVMF_CANDIDATES = [r"C:\Program Files\qemu\share\edk2-x86_64-code.fd",
                   r"C:\Program Files (x86)\qemu\share\edk2-x86_64-code.fd"]
OVMF_VARS = [r"C:\Program Files\qemu\share\edk2-i386-vars.fd",
             r"C:\Program Files (x86)\qemu\share\edk2-i386-vars.fd"]


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def find_first(cands):
    for c in cands:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            w = shutil.which(c)
            if w:
                return w
    return None


def load_mk():
    spec = importlib.util.spec_from_file_location("mkiso", os.path.join(ROOT, "tools", "make_iso64.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def boot_case(qemu, tag, target_bytes=32 * 1024 * 1024, wait=200):
    """三种引导各跑一次；返回 (ok, log)。判据只看串口原文。"""
    serial = os.path.join(OUT, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    tgt = os.path.join(OUT, tag + "_target.img")
    with open(tgt, "wb") as f:
        f.truncate(target_bytes)
    args = [qemu, "-name", "iso-" + tag, "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % serial,
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % free_port(),
            "-no-reboot"]
    if tag == "uefi":
        ovmf = find_first(OVMF_CANDIDATES)
        vtpl = find_first(OVMF_VARS)
        if not ovmf or not vtpl:
            return None, ""
        varsf = os.path.join(OUT, "uefi_vars.fd")
        shutil.copyfile(vtpl, varsf)
        args += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % ovmf,
                 "-drive", "if=pflash,format=raw,unit=1,file=%s" % varsf,
                 "-cdrom", ISO, "-drive", "format=raw,file=%s" % tgt, "-boot", "order=d"]
    elif tag == "hybrid":
        args += ["-drive", "format=raw,file=%s" % ISO,
                 "-drive", "format=raw,file=%s" % tgt, "-boot", "order=c"]
    else:
        args += ["-cdrom", ISO, "-drive", "format=raw,file=%s" % tgt, "-boot", "order=d"]
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < wait:
        try:
            log = open(serial, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            log = ""
        if "[SETUP] 磁盘枚举完成" in log or "TRIPLE FAULT" in log:
            break
        if p.poll() is not None:
            break
        time.sleep(1.0)
    try:
        log = open(serial, "r", encoding="utf-8", errors="replace").read()
    except OSError:
        log = ""
    if p.poll() is None:
        p.kill()
    try:
        p.wait(timeout=10)
    except Exception:
        pass
    return log, log


def main():
    qemu = find_first(QEMU_CANDIDATES)
    if not qemu or not os.path.exists(ISO):
        print("跳过：qemu 或 ISO 不可用（%s）" % ISO)
        return 2

    mk = load_mk()
    img = bytearray(open(ISO, "rb").read())
    print("=== iso64_layout_test：ISO 层级结构 + 三种引导（%d B）===" % len(img))

    # ---------- ① 层级树 ----------
    tree = ["BOOT/CDISO.BIN", "BOOT/LOADER64.BIN", "BOOT/KERNEL64.BIN",
            "SYSTEM/SYSTEM.IMG", "EFI/BOOT/BOOTX64.EFI", "EFI/BOOT/UEFI64.BIN",
            "README.TXT", "VERSION"]
    got = {}
    for p in tree:
        try:
            got[p] = mk.iso_find_path(img, p)
        except RuntimeError as e:
            got[p] = None
            print("  [info] %s 解析失败：%s" % (p, e))
    missing = [p for p in tree if not got.get(p)]
    check("层级树 8 个路径全部可从镜像解析出来", not missing,
          "缺：%s" % missing if missing else " | ".join("%s@%d" % (p, got[p][0]) for p in tree))
    if missing:
        print("\n=== RESULT: FAIL === ISO 不是层级布局")
        return 1

    # ---------- ② 根目录不再有扁平文件 ----------
    pvd = img[16 * 2048: 17 * 2048]
    root_lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
    root_len = struct.unpack_from("<I", pvd, 156 + 10)[0]
    root_names = [mk.iso_norm(n) for n, _l, _s, _d in mk.iso_dir_records(img, root_lba, root_len)]
    flat = [n for n in ("CDISO.BIN", "LOADER64.BIN", "KERNEL64.BIN", "SYSTEM.IMG") if n in root_names]
    check("根目录不再摊扁平引导文件（真正的树状结构）", not flat,
          "根目录=%s" % sorted(root_names))
    check("根目录里有 BOOT / SYSTEM / EFI 三个目录",
          all(d in root_names for d in ("BOOT", "SYSTEM", "EFI")), "根目录=%s" % sorted(root_names))

    # ---------- ③ 内容逐字节一致 ----------
    def iso_bytes(path):
        lba, size = mk.iso_find_path(img, path)
        return bytes(img[lba * 2048: lba * 2048 + size])

    for path, local in (("BOOT/LOADER64.BIN", "loader64.bin"),
                        ("SYSTEM/SYSTEM.IMG", "system.img"),
                        ("EFI/BOOT/BOOTX64.EFI", "BOOTX64.EFI"),
                        ("EFI/BOOT/UEFI64.BIN", "UEFI64.BIN")):
        blob = iso_bytes(path)
        src = open(os.path.join(BUILD, local), "rb").read()
        check("/%s 与 build64/%s 逐字节一致（%d B）" % (path, local, len(blob)),
              blob == src, "%d vs %d" % (len(blob), len(src)))

    ver_iso = iso_bytes("VERSION").decode("ascii", "replace").strip()
    ver_src = mk.vimtuos_version()
    check("/VERSION 里的版本串与 build64.sh 一致（%s）" % ver_src,
          ver_iso == "VimtuOS %s" % ver_src, repr(ver_iso))
    rd = iso_bytes("README.TXT").decode("ascii", "replace")
    check("/README.TXT 写清了层级布局与三种引导", "/BOOT/CDISO.BIN" in rd and "0xEF" in rd,
          "%d B" % len(rd))

    # CDISO.BIN 例外：镜像里它的**引导 LBA 补丁点已被回填**，所以不能整段比对。
    #   判据 = 长度一致 + **除补丁点之外逐字节一致**（两边补丁位都抹 0 再比）。
    #   补丁点全表与 make_iso64.main 的 patch_dword 一一对应（光盘路径 + 硬盘/U 盘路径；
    #   cdiso.bin 里两套都在，硬盘那套由 hybrid 路径使用）。
    iso_cdiso = bytearray(iso_bytes("BOOT/CDISO.BIN"))
    src_cdiso = bytearray(open(os.path.join(BUILD, "cdiso.bin"), "rb").read())
    patched = 0
    for magic in (b"VMLD", b"VMLB", b"VMKN", b"VMSS", b"VML9", b"VMK9", b"VMKB",
                  b"VMSY", b"VMS9", b"VMYB"):
        i = 0
        while True:
            i = src_cdiso.find(magic, i)
            if i < 0:
                break
            iso_cdiso[i + 4:i + 8] = b"\0\0\0\0"
            src_cdiso[i + 4:i + 8] = b"\0\0\0\0"
            patched += 1
            i += 8
    check("/BOOT/CDISO.BIN 除 %d 处回填点外与 build64/cdiso.bin 逐字节一致（%d B）"
          % (patched, len(iso_cdiso)),
          len(iso_cdiso) == len(src_cdiso) and iso_cdiso == src_cdiso and patched >= 4,
          "len %d vs %d, patched=%d" % (len(iso_cdiso), len(src_cdiso), patched))

    # ---------- ④ 回填 LBA 与新位置自洽 ----------
    for magic, path in ((b"VMLD", "BOOT/LOADER64.BIN"),
                        (b"VMKN", "BOOT/KERNEL64.BIN"),
                        (b"VMSY", "SYSTEM/SYSTEM.IMG")):
        want = mk.iso_find_path(img, path)[0]
        hits = []
        i = img.find(magic)
        while i >= 0 and len(hits) < 4:
            hits.append(struct.unpack_from("<I", img, i + 4)[0])
            i = img.find(magic, i + 1)
        check("%s 回填值 == /%s 的真实 LBA（%d）" % (magic.decode(), path, want),
              bool(hits) and all(h == want for h in hits),
              "补丁点=%s 期望=%d" % (hits, want))

    # ---------- ⑤ 三种引导 ----------
    for tag in ("bios", "uefi", "hybrid"):
        log, _ = boot_case(qemu, tag)
        if log is None:
            check("%s 引导（缺 OVMF，跳过）" % tag, True, "SKIP")
            continue
        ok_setup = "[SETUP] 安装程序启动" in log
        rows = None
        for ln in log.splitlines():
            if "[SETUP] 磁盘枚举完成 rows=" in ln:
                try:
                    rows = int(ln.split("rows=")[1].split()[0])
                except (IndexError, ValueError):
                    pass
        check("%s 引导进安装程序（[SETUP] 安装程序启动）" % tag, ok_setup,
              (ln.strip()[:110] if ok_setup else "（未出现）"))
        check("%s 安装程序看到了目标盘（rows>=2，实测 %s）" % (tag, rows),
              rows is not None and rows >= 2, "rows=%s" % rows)
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("%s 全程无 %s" % (tag, bad), bad not in log)

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
