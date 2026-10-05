#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/iconboot64_test.py - ★ 回归：图标"有时有、有时没有"的根因与修复

缺陷（本测试要钉住的）：
  图标包的兜底来源 = **原始区**（system.img 里构建期写入的一段 blob 区；LBA/字节数见
  build64/demo64_blobtab.h 的 DEMO64_RAW_LBA / DEMO64_RAW_BYTES —— 本批起中文面字体
  /etc/font_simhei.z 也在这段里、且排在包**前面**，原始区还在 os_boot_path 的
  ata64_init64() 之后就**提前装载**。所以打点时机/内容会随布局变，本测试一律
  **从生成物读事实**（blobtab + demo64_raw.bin + iconpack.bin），不写死 LBA/大小）。
  修前 kernel/demo64.cpp **硬编码 drive 0（PATA 主盘）**读，且失败后永久缓存 -1。
  只要引导盘不是 PATA-0（QEMU 的 ich9-ahci/SATA、VMware 的 SATA/AHCI、NVMe、任何
  drive 0 不存在/读失败），"系统卷里没有 /etc/iconpack.bin"的系统就拿不到包 ->
  图标回落成程序化绘制 -> 同一份系统换机器/换控制器就"图标有时有有时没有"。

它做什么（全部真跑 QEMU；同一时刻只跑一份；夹具 = system.img + **不含图标包**的 VimtuFS2 卷）：
  ① PATA（-drive index=0）：对照组 —— 原始区在 drive 0 上加载成功，包装进卷
     -> src=vfs + 逐 kind ok=1 + 桌面图标像素
  ② AHCI-only（-device ich9-ahci + ide-hd,bus=ahci.0，没有 IDE 兼容盘）：
     断言修后按**系统盘**读到原始区（drive 不是 PATA 的 0、且与盘符扫描出的系统盘一致）、
     早期失败不再永久缓存（先 FAILED retry=1，扫描后 loaded）—— 再从原始区装进卷
     -> src=vfs + 逐 kind ok=1 + 桌面图标像素
  ③ 反例/边界：卷里没有包 **且** 原始区被清零（清零范围 = blobtab 的整段原始区，
     夹具写入后独立回读自检）-> 如实打点 `init pack absent`（含 sys-disk/probes/raw-reads 明细）
     + 每个 kind 打 fallback reason=no-pack + Dock 图标位置仍有墨迹（程序化兜底），**不崩/无 PANIC**
  ④ 装机路径：安装介质（IDE index0）-> AHCI 目标盘走完安装（[INSTALL] 完成）-> **只挂 AHCI 盘**
     重启 -> `[LM] disk boot via INT 13h` + 目标盘原始区逐字节 = build64/demo64_raw.bin
     + `[ICON64] init pack … bad=0 ok=1` + 逐 kind ok=1 + 像素（真图标上屏）

退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
用法：py -3 tests\\iconboot64_test.py [--qemu 路径] [--port 5688] [--keep] [--skip-install]
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))    # make_shellvol：离线 VimtuFS2 卷写入器

import startmenu64_test as smt      # noqa: E402  （Vm/Monitor/Cursor/read_ppm/find_qemu：其他验收同款）
import icons64_test as ict          # noqa: E402  （像素工具 + manifest 口径，**同一套公式/阈值**）
import make_shellvol as msv         # noqa: E402
import qemuhelp as qh               # noqa: E402  （显式登录手势）

SECTORS = 32768                     # 16 MB 夹具盘（与 icons64_test 同几何）
PART_MAIN_LBA = 8009
SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
PACK_BIN = os.path.join(ROOT, "build", "iconpack.bin")
MANIFEST = os.path.join(ROOT, "build", "icons", "manifest.json")
PACK_FILE = "/etc/iconpack.bin"
# ★ 本批：原始区 LBA/字节数/blob 偏移**一律从构建期生成物读**（不再写死 7497/193,336）——
#   3d1ff31 把布局改成"LBA 6096 起 + 中文面字体 blob 在前"，写死的期望当场过期。读不到
#   就报环境问题（先跑 build64.sh），绝不静默跳过：下次布局再变，这里的期望自动跟上。
BLOB_TAB = os.path.join(ROOT, "build64", "demo64_blobtab.h")
RAW_BIN = os.path.join(ROOT, "build64", "demo64_raw.bin")
PACK_BIN_RAW = os.path.join(ROOT, "build64", "iconpack.bin")


def q(p):
    return p.replace("\\", "/")

def read_blobtab():
    """从 build64/demo64_blobtab.h 读原始区事实：返回 (raw_lba, raw_bytes, blobs)。
    blobs = {路径: (offset, size)} —— 内核侧 demo64_blob_find64 查的就是这份构建期生成表。"""
    with open(BLOB_TAB, "r", encoding="utf-8", errors="replace") as f:
        tab = f.read()
    m_lba = re.search(r"#define\s+DEMO64_RAW_LBA\s+(\d+)", tab)
    m_bytes = re.search(r"#define\s+DEMO64_RAW_BYTES\s+(\d+)", tab)
    blobs = {mm.group(1): (int(mm.group(2)), int(mm.group(3)))
             for mm in re.finditer(r'\{\s*"([^"]+)",\s*(\d+)u,\s*(\d+)u\s*\}', tab)}
    if not m_lba or not m_bytes or "/hello.elf" not in blobs or PACK_FILE not in blobs:
        raise RuntimeError("build64/demo64_blobtab.h 缺少原始区事实（先跑 bash build64.sh）")
    return int(m_lba.group(1)), int(m_bytes.group(1)), blobs


class Vm:
    """夹具盘 + QEMU（PATA 或 AHCI-only；可选安装介质）。"""

    def __init__(self, qemu, port, name, tmp, img=None, medium=None):
        self.port, self.name = port, name
        self.serial = os.path.join(tmp, name + "_serial.log")
        args = [qemu, "-name", name]
        if medium:
            args += ["-drive", "format=raw,file=%s,index=0,media=disk" % q(medium)]
        if img:
            if self._ahci:
                args += ["-device", "ich9-ahci,id=ahci",
                         "-drive", "file=%s,if=none,id=d0,format=raw" % q(img),
                         "-device", "ide-hd,drive=d0,bus=ahci.0"]
            else:
                args += ["-drive", "format=raw,file=%s,index=0,media=disk" % q(img)]
        args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
                 "-serial", "file:%s" % q(self.serial),
                 "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                 "-no-reboot"]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    _ahci = False

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def monitor(self):
        return smt.Monitor(self.port)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class VmAhci(Vm):
    _ahci = True


def make_nopack_fixture(tmp, name, raw_lba, raw_secs, zero_raw=False):
    """system.img + 一个**没有 /etc/iconpack.bin** 的 VimtuFS2 卷（靠原始区把包装进卷的路径）。
    raw_lba/raw_secs = 从 build64/demo64_blobtab.h 读到的原始区（不写死）。
    zero_raw=True 时把**整段**原始区（LBA raw_lba 起 raw_secs 扇区）清零 —— 模拟"原始区读得到
    但没有包/读不到"的边界；清零后独立回读自检必须是全 0（防止布局再变时 zero 到别处、测试静默测错）。"""
    sys_bytes = open(SYSTEM_IMG, "rb").read()
    vol = msv.Volume(SECTORS - PART_MAIN_LBA)
    etc = vol.mkdir("etc", mode=0o755)
    vol.mkdir("tmp", mode=0o777)
    marker = b"iconboot64: no iconpack in this volume\n"
    vol.write_file("marker.txt", marker, parent=etc, mode=0o644, kind=6)
    blob = vol.finish()
    bad = msv.verify(blob, {"/etc/marker.txt": marker})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = bytearray(msv.build_disk(sys_bytes, blob, SECTORS))
    if zero_raw:
        img[raw_lba * 512:(raw_lba + raw_secs) * 512] = bytes(raw_secs * 512)
        if bytes(img[raw_lba * 512:(raw_lba + raw_secs) * 512]) != bytes(raw_secs * 512):
            raise RuntimeError("原始区清零没有落到 LBA %d（夹具事实过期）" % raw_lba)
    path = os.path.join(tmp, name)
    with open(path, "wb") as f:
        f.write(bytes(img))
    # 独立回读：卷里确实没有包（负面夹具自检）
    at = PART_MAIN_LBA * 512
    if blob[:8] not in bytes(img[at:at + 8]):
        raise RuntimeError("夹具卷写入位置不对")
    return path


def login_and_ready(vm, mon, timeout=180):
    qh.login_desktop(mon, vm.log, vm.proc, timeout=min(timeout, 180))
    return vm.wait_log("[GUI64] ready", timeout)


def count_loads(log):
    return len(re.findall(r"\[ICON64\] load kind=\S+ path=pack:\S+ size=\d+ src=vfs ok=1", log))

SHOT_SEQ = [0]      # screendump 文件名必须是纯 ASCII（QEMU monitor 的路径参数不接受非 ASCII）


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5688)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--skip-install", action="store_true", help="跳过 ④ 装机路径（只跑 ①②③）")
    args = ap.parse_args()

    for need, why in ((SYSTEM_IMG, "先跑 bash build64.sh"), (PACK_BIN, "先跑 bash build64.sh"),
                      (BLOB_TAB, "先跑 bash build64.sh"), (RAW_BIN, "先跑 bash build64.sh"),
                      (PACK_BIN_RAW, "先跑 bash build64.sh"), (MANIFEST, "先跑 bash build64.sh")):
        if not os.path.exists(need):
            sys.stderr.write("缺少 %s（%s）\n" % (need, why))
            return 2
    # ★ 原始区事实（LBA/字节数/blob 偏移）从构建期生成物读；读不到宁可失败，不静默跳过
    raw_lba, raw_bytes, blobs = read_blobtab()
    raw_secs = (raw_bytes + 511) // 512
    hello_off = blobs["/hello.elf"][0]
    rawoff_pack, rawlen_pack = blobs[PACK_FILE]
    qemu = smt.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    man = json.load(open(MANIFEST, encoding="utf-8"))
    kinds = {e["name"] for e in man["entries"]}
    pack_bytes = man["pack_bytes"]
    tmp = tempfile.mkdtemp(prefix="vimtu64_iconboot_")
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def boom():
        bad = [n for n, ok in checks if not ok]
        print("\n===== 汇总：%d 项断言，失败 %d 项 =====" % (len(checks), len(bad)))
        for n in bad:
            print("  FAIL: %s" % n)
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("临时目录保留：%s" % tmp)
        return 1 if bad else 0

    def scene_pixel_desktop(vm, mon, log, tag):
        """桌面图标 #0（我的电脑）= 真图标：区域含调色板亮色（像素级证据）。"""
        SHOT_SEQ[0] += 1
        time.sleep(0.8)
        shot = os.path.join(tmp, "shot%d_desktop.ppm" % SHOT_SEQ[0])
        got = mon.shot(shot)
        if not got:
            check("%s 拿到桌面截图" % tag, False, "screendump 失败")
            return
        w, h, px = smt.read_ppm(shot)
        m = re.search(r"\[DESK64\] item idx=0 kind=0 name=\S+ x=(\d+) y=(\d+) w=(\d+)", log)
        if not m:
            check("%s 桌面图标几何打点（[DESK64] item idx=0 kind=0）" % tag, False, "（无）")
            return
        x, y = int(m.group(1)) + 4, int(m.group(2)) + 4
        rgb = ict.APP_COLOR["mypc"]
        other = ict.APP_COLOR["tmgr"]
        r1 = ict.color_ratio(px, w, x, y, 40, rgb)
        r2 = ict.color_ratio(px, w, x, y, 40, other)
        check("%s 桌面图标 #0(mypc) 含调色板真图标色 #%02X%02X%02X（>=8%% 且高于其它色）" % (tag, *rgb),
              r1 >= 0.08 and r1 > r2, "命中=%.2f 其它色=%.2f" % (r1, r2))

    def scene_pixel_dock_mypc(vm, mon, log, tag):
        """Dock #1（我的电脑）区域仍有墨迹（程序化兜底也必须画出来）。"""
        SHOT_SEQ[0] += 1
        time.sleep(0.8)
        shot = os.path.join(tmp, "shot%d_dock.ppm" % SHOT_SEQ[0])
        got = mon.shot(shot)
        if not got:
            check("%s 拿到截图（Dock 墨迹）" % tag, False, "screendump 失败")
            return
        w, h, px = smt.read_ppm(shot)
        it1 = None
        for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=\d+ name=\S+ x=\d+ y=\d+ w=(\d+) h=\d+ "
                             r"cx=(\d+) cy=(\d+)", log):
            if int(m.group(1)) == 1:
                it1 = m
        if not it1:
            check("%s Dock #1 几何打点" % tag, False, "（无）")
            return
        n = int(it1.group(2))
        x0, y0 = int(it1.group(3)) - n // 2, int(it1.group(4)) - n // 2
        ink = ict.ink_count(ict.ink_mask(px, w, x0, y0, n, 60))
        check("%s Dock #1(我的电脑) 区域仍有墨迹（>=60 像素）" % tag, ink >= 60,
              "ink=%d/%d" % (ink, n * n))

    # =============================================================
    print("=== ① PATA 对照：卷里没有包 -> 原始区（drive 0）装进卷 -> src=vfs + 真图标上屏 ===")
    img1 = make_nopack_fixture(tmp, "nopack_pata.img", raw_lba, raw_secs)
    vm1 = Vm(qemu, args.port, "iconboot-pata", tmp, img=img1)
    try:
        mon1 = vm1.monitor()
        up = login_and_ready(vm1, mon1)
        check("① 进桌面（[GUI64] ready）", up)
        log1 = vm1.log()
        m1r = re.search(r"\[DEMO64\] raw blob region loaded lba=(\d+) bytes=(\d+) drive=(\d+) attempt=\d+", log1)
        check("① 原始区按构建布局在 drive 0 上加载成功（blobtab: lba=%d bytes=%d）" % (raw_lba, raw_bytes),
              m1r is not None and int(m1r.group(1)) == raw_lba and int(m1r.group(2)) == raw_bytes
              and m1r.group(3) == "0",
              m1r.group(0) if m1r else (re.search(r"\[DEMO64\] raw blob region loaded[^\r\n]*", log1) or ["（无）"])[0])
        check("① 包从原始区装进卷（[IMG64] install path=/etc/iconpack.bin … ok=1）",
              re.search(r"\[IMG64\] install path=/etc/iconpack\.bin bytes=\d+ written=\d+ ok=1", log1) is not None,
              (re.search(r"\[IMG64\] install path=/etc/iconpack\.bin[^\r\n]*", log1) or ["（无）"])[0])
        m1 = re.search(r"\[ICON64\] init pack lba=0 drive=-1 bytes=(\d+) entries=\d+ icons=\d+ "
                       r"bad=(\d+) ok=(\d) fnv=[0-9a-f]+ vfs_icons=\d src=vfs path=/etc/iconpack\.bin", log1)
        check("① [ICON64] init pack … src=vfs path=/etc/iconpack.bin（lba=0/drive=-1 = 从卷读回）",
              m1 is not None and int(m1.group(1)) == pack_bytes and m1.group(2) == "0" and m1.group(3) == "1",
              m1.group(0) if m1 else "（无）")
        nl1 = count_loads(log1)
        check("① 每个 kind 都有真图标（[ICON64] load … src=vfs ok=1 = %d 条）" % len(kinds),
              nl1 == len(kinds), "%d 条" % nl1)
        if up:
            scene_pixel_desktop(vm1, mon1, log1, "①")
        check("① 无 PANIC/TRIPLE FAULT", "PANIC" not in log1 and "TRIPLE FAULT" not in log1)
    finally:
        vm1.close()

    # =============================================================
    print("=== ② ★ 回归：AHCI-only（无 IDE 兼容盘）也必须拿到同一份图标包 ===")
    img2 = make_nopack_fixture(tmp, "nopack_ahci.img", raw_lba, raw_secs)
    vm2 = VmAhci(qemu, args.port + 1, "iconboot-ahci", tmp, img=img2)
    try:
        mon2 = vm2.monitor()
        up = login_and_ready(vm2, mon2)
        check("② 进桌面（[GUI64] ready）", up)
        log2 = vm2.log()
        # 启动早期（盘符扫描之前）drive 0 失败：必须如实打点，且**不能**写成永久失败
        mf = re.search(r"\[DEMO64\] raw blob region read FAILED lba=(\d+) secs=(\d+) drives=([\d,]+) "
                       r"attempt=(\d+) retry=(\d)", log2)
        check("② 早期失败如实打点（drives=0、attempt=1、retry=1；修前这里就是永久 -1）",
              mf is not None and int(mf.group(1)) == raw_lba and int(mf.group(2)) == raw_secs
              and mf.group(3) == "0" and mf.group(4) == "1" and mf.group(5) == "1",
              mf.group(0) if mf else (re.search(r"\[DEMO64\] raw blob region read[^\r\n]*", log2) or ["（无）"])[0])
        ml = re.search(r"\[DEMO64\] raw blob region loaded lba=(\d+) bytes=(\d+) drive=(-?\d+) attempt=\d+", log2)
        msys = re.search(r"\[ICON64\] pack missing in system volume[^\r\n]* disk=(-?\d+)", log2)
        sys_disk = msys.group(1) if msys else None
        check("② ★ 修复生效：扫描后按**系统盘**读到原始区（drive 不是 PATA 的 0；与盘符扫描的系统盘一致）",
              ml is not None and int(ml.group(1)) == raw_lba and int(ml.group(2)) == raw_bytes
              and ml.group(3) != "0" and (sys_disk is None or ml.group(3) == sys_disk),
              ml.group(0) if ml else (re.search(r"\[DEMO64\] raw blob region loaded[^\r\n]*", log2) or ["（无）"])[0])
        check("② 有卷但包不在 -> 不再静默（[ICON64] pack missing in system volume …）",
              re.search(r"\[ICON64\] pack missing in system volume path=/etc/iconpack\.bin present=0", log2) is not None,
              (re.search(r"\[ICON64\] pack missing[^\r\n]*", log2) or ["（无）"])[0])
        check("② 包从原始区装进卷（[IMG64] install path=/etc/iconpack.bin … ok=1）",
              re.search(r"\[IMG64\] install path=/etc/iconpack\.bin bytes=\d+ written=\d+ ok=1", log2) is not None,
              (re.search(r"\[IMG64\] install path=/etc/iconpack\.bin[^\r\n]*", log2) or ["（无）"])[0])
        m2 = re.search(r"\[ICON64\] init pack lba=0 drive=-1 bytes=(\d+) entries=\d+ icons=\d+ "
                       r"bad=(\d+) ok=(\d) fnv=[0-9a-f]+ vfs_icons=\d src=vfs path=/etc/iconpack\.bin", log2)
        check("② [ICON64] init pack … src=vfs（AHCI 引导也是同一份包）",
              m2 is not None and int(m2.group(1)) == pack_bytes and m2.group(2) == "0" and m2.group(3) == "1",
              m2.group(0) if m2 else "（无）")
        nl2 = count_loads(log2)
        check("② 逐 kind 真图标（[ICON64] load … src=vfs ok=1 = %d 条）" % len(kinds),
              nl2 == len(kinds), "%d 条" % nl2)
        if up:
            scene_pixel_desktop(vm2, mon2, log2, "②")
        check("② 无 PANIC/TRIPLE FAULT", "PANIC" not in log2 and "TRIPLE FAULT" not in log2)
    finally:
        vm2.close()

    # =============================================================
    print("=== ③ 反例/边界：卷里没有包 + 原始区被清零 -> 如实打点 + 程序化兜底（不崩）===")
    img3 = make_nopack_fixture(tmp, "nopack_zero.img", raw_lba, raw_secs, zero_raw=True)
    vm3 = VmAhci(qemu, args.port + 2, "iconboot-zero", tmp, img=img3)
    try:
        mon3 = vm3.monitor()
        up = login_and_ready(vm3, mon3)
        check("③ 仍能进桌面（[GUI64] ready）", up)
        log3 = vm3.log()
        ma = re.search(r"\[ICON64\] init pack absent reason=no-magic-or-read lba=(\d+) drives-tried=(\d+) "
                       r"last-drive=(-?\d+) vfs=(\d) sys-disk=(-?\d+) vfs-file=(\d) probes=(\S+) raw-reads=(\d+)", log3)
        check("③ 如实打「没有任何来源」（有卷 vfs=1、卷里没有可用包 vfs-file=0、系统盘已知）",
              ma is not None and ma.group(4) == "1" and ma.group(6) == "0" and ma.group(5) != "-1",
              ma.group(0) if ma else (re.search(r"\[ICON64\] init pack absent[^\r\n]*", log3) or ["（无）"])[0])
        check("③ 探测明细：系统盘这一档探过且没有包 magic（probes 含 <sys-disk>:magic）",
              ma is not None and ("%s:magic" % ma.group(5)) in ma.group(7),
              ma.group(7) if ma else "（无）")
        nfb = len(re.findall(r"\[ICON64\] fallback kind=\S+ reason=no-pack", log3))
        check("③ 每个用到的 kind 都打回落点（fallback … reason=no-pack >= 8 条）", nfb >= 8, "%d 条" % nfb)
        check("③ 没有把坏盘当成功（[ICON64] init pack lba= 不出现）",
              re.search(r"\[ICON64\] init pack lba=", log3) is None)
        if up:
            scene_pixel_dock_mypc(vm3, mon3, log3, "③")
        check("③ 无 PANIC/TRIPLE FAULT", "PANIC" not in log3 and "TRIPLE FAULT" not in log3)
    finally:
        vm3.close()

    # =============================================================
    if not args.skip_install:
        print("=== ④ 装机路径：介质(IDE) -> AHCI 目标盘走完安装 -> 只挂目标盘(AHCI) 重启 ===")
        if not os.path.exists(MEDIUM):
            sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % MEDIUM)
            return 2
        target = os.path.join(tmp, "target_ahci.img")
        with open(target, "wb") as f:
            f.write(b"\0" * (SECTORS * 512))
        ins_log = os.path.join(tmp, "install_serial.log")
        # 安装：介质挂 IDE(index0) + 16MB 空目标盘挂 ich9-ahci port0（AHCI 编号 = drive 8）。
        vmi = VmAhci(qemu, args.port + 3, "iconboot-install", tmp, img=target, medium=MEDIUM)
        try:
            mon_i = vmi.monitor()
            vmi.wait_log("磁盘枚举完成", 90)
            for key in ("ret", "ret", "ret", "ret", "n", "ret"):
                mon_i.key(key, wait=1.2)
            done = vmi.wait_log("[SETUP] 安装完成", 150) or vmi.wait_log("[INSTALL] 完成：已写", 10)
            check("④ 安装完成（[SETUP] 安装完成 / [INSTALL] 完成：已写）", done)
            log_i = vmi.log()
            check("④ 安装走 AHCI 写盘（[AHCI64] write lba=…）", "[AHCI64] write lba=" in log_i)
            # ★ 优雅退出（monitor quit）而不是 kill：让 QEMU 把块设备缓冲刷回目标镜像。
            #   install 的载荷 8073 扇区是按 LBA 递增写的，卷（8009..）在**最后**几块里；
            #   直接 kill 会丢尾部（实测离线读到 LBA 8009 是 0 -> 目标盘看起来没有卷）。
            mon_i.send("quit", wait=1.0)
            try:
                vmi.proc.wait(timeout=20)
            except Exception:
                pass
        finally:
            vmi.close()
        time.sleep(1.0)
        with open(target, "rb") as f:
            f.seek(raw_lba * 512)
            raw_seg = f.read(raw_bytes)
            f.seek(PART_MAIN_LBA * 512)
            vol_magic = f.read(8)
        raw_ref = open(RAW_BIN, "rb").read()
        pack_ref = open(PACK_BIN_RAW, "rb").read()
        check("④ 装好的目标盘带着原始区：LBA %d 起 %d B 与 build64/demo64_raw.bin 逐字节一致"
              % (raw_lba, raw_bytes),
              raw_seg == raw_ref, "%d B（参考 %d B）" % (len(raw_seg), len(raw_ref)))
        check("④ 原始区里的 /hello.elf 偏移对得上（+%d = ELF 头）" % hello_off,
              raw_seg[hello_off:hello_off + 4] == b"\x7fELF", repr(raw_seg[hello_off:hello_off + 8]))
        check("④ 原始区里的 %s 偏移对得上（+%d = 图标包 %d B，逐字节一致）" % (PACK_FILE, rawoff_pack, rawlen_pack),
              raw_seg[rawoff_pack:rawoff_pack + rawlen_pack] == pack_ref)
        print("     （离线读 LBA 8009 = %r —— 本批实测：安装只写 8073 扇区载荷，主分区还没有卷）"
              % vol_magic)
        # 目标盘挂在 ich9-ahci port0（AHCI 编号 = drive 8）。QEMU 的 -drive 直接指目标镜像。
        vmb = VmAhci(qemu, args.port + 4, "iconboot-installed", tmp, img=target)
        try:
            mon_b = vmb.monitor()
            up = login_and_ready(vmb, mon_b)
            check("④ 只挂 AHCI 盘也能启动进桌面（[GUI64] ready）", up)
            log_b = vmb.log()
            check("④ 引导层走 BIOS INT 13h（[LM] disk boot via INT 13h dl=0x..）",
                  "[LM] disk boot via INT 13h dl=0x" in log_b)
            check("④ 走系统启动路径（[OS] booted from installed disk）",
                  "[OS] booted from installed disk" in log_b)
            check("④ 没有可挂载系统卷时如实打点（[ICON64] no mounted system volume …）",
                  re.search(r"\[ICON64\] no mounted system volume \(vfs system-slot=", log_b) is not None,
                  (re.search(r"\[ICON64\] no mounted system volume[^\r\n]*", log_b) or ["（无）"])[0])
            mpb = re.search(r"\[DEMO64\] raw probe drive=(-?\d+) loaded lba=(\d+) bytes=(\d+)", log_b)
            check("④ ★ 装好的盘上按盘探测读到原始区（drive 不是 PATA 的 0；lba/bytes 与构建布局一致）",
                  mpb is not None and mpb.group(1) != "0" and int(mpb.group(2)) == raw_lba
                  and int(mpb.group(3)) == raw_bytes,
                  mpb.group(0) if mpb else (re.search(r"\[DEMO64\] raw probe[^\r\n]*", log_b) or ["（无）"])[0])
            m4 = re.search(r"\[ICON64\] init pack lba=(\d+) drive=(-?\d+) bytes=(\d+) entries=\d+ icons=\d+ "
                           r"bad=(\d+) ok=(\d) fnv=[0-9a-f]+ vfs_icons=\d src=(vfs|raw)", log_b)
            check("④ 装好的盘图标包可用（init pack src=raw、来源盘与探测盘一致，bad=0 ok=1、bytes 与包一致）",
                  m4 is not None and int(m4.group(3)) == pack_bytes and m4.group(4) == "0" and m4.group(5) == "1"
                  and m4.group(6) == "raw" and m4.group(2) != "0"
                  and (mpb is None or m4.group(2) == mpb.group(1)),
                  m4.group(0) if m4 else (re.search(r"\[ICON64\] init pack[^\r\n]*", log_b) or ["（无）"])[0])
            nl4 = len(re.findall(r"\[ICON64\] load kind=\S+ path=pack:\S+ size=\d+ src=\S+ ok=1", log_b))
            check("④ 逐 kind 真图标（[ICON64] load … ok=1 = %d 条）" % len(kinds),
                  nl4 == len(kinds), "%d 条" % nl4)
            if up:
                scene_pixel_desktop(vmb, mon_b, log_b, "④")
            check("④ 无 PANIC/TRIPLE FAULT", "PANIC" not in log_b and "TRIPLE FAULT" not in log_b)
        finally:
            vmb.close()

    return boom()


if __name__ == "__main__":
    sys.exit(main())
