#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/drm64_test.py - ★ v0.9.12：DRM/KMS + evdev 内核门面 + ring3 验收程序 + 混合呈现

要证明的事（每条都有串口/宿主侧字节证据）：
  ① **两份头文件逐值一致**：kernel/drm64.h 与 user/lib/drm64.h 的 `#define` 数值 +
     每个结构体的字段序列完全相同（ABI 只有一个真源，拷贝必须同步）。
  ② 内核自检：`[DRM] selftest dumb=ok flip=ok vblank=<n> mode=<hw|soft> ok=1`
     （建 dumb -> 写测试图案 -> 内核读回核对 [DRM] verify ok=1 -> page flip -> 等 vblank ->
       把开机画面翻回来 -> 销毁）。自检**不改变**开机画面（快照翻回）。
  ③ 混合呈现与 vblank：`[DRM] present ... path=gpu|cpu`（有设备走 virtio-gpu 2D，否则 CPU 写 LFB），
     vblank 序列号单调递增（多次 wait 的 seq 严格变大）。
  ④ ring3 验收程序（/bin/drmdemo.elf，真进程）：[DRMDEMO] 各阶段行齐全，未知 ioctl 必须
     `badcmd rc=-25`（-ENOTTY 反例），最后 `[DRMDEMO] done ok=1`；
     **ring3 能写那块 dumb buffer**：`paint ... probe=0xffffffff`（读回的是自己写进去的白色条）+ 
     内核侧 `[DRM] map ... u=1`。
  ⑤ 桌面照旧：`[GUI64] ready`（登录后），screendump 上部非黑像素比例 > 0.3（照 gpu64_test 的
     nonblack_frac）。
  ⑥ 禁止项：PANIC / TRIPLE FAULT / `[DRM] selftest FAIL` / `[DRMDEMO] done ok=0`。

夹具：build64/sysdisk.img 副本 + 卷里写 /bin/drmdemo.elf（build64/user_drmdemo.elf）+
      /etc/drm64_probe（演示开关，与 /etc/wl64_probe、/etc/wm_probe 同一套约定）。
      没有 argv/probe 时内核只打一行 skipped、不画一个像素。

用法（必须 Windows 原生 Python）：py -3 tests\\drm64_test.py [--timeout 240] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import importlib.util
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
import qemuhelp as qh              # noqa: E402

BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")            # 完整演示盘（夹具底座）
DRMDEMO_ELF = os.path.join(BUILD, "user_drmdemo.elf")  # user/build_user.sh 的产物
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[DRM] selftest FAIL",
    "[DRMDEMO] done ok=0",
]


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def q(p):
    return p.replace("\\", "/")


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            f = shutil.which(c)
            if f:
                return f
    return None


# ---------------------------------------------------------------------------
# 头文件逐值比对（ABI 只有一个真源）
# ---------------------------------------------------------------------------
STRUCT_NAMES = [
    "Drm64Version", "Drm64GetCap", "Drm64ModeResources", "Drm64ModeMode",
    "Drm64ModeCrtc", "Drm64ModeConnector", "Drm64ModeCreateDumb",
    "Drm64ModeMapDumb", "Drm64ModeDestroyDumb", "Drm64ModePageFlip",
    "Drm64WaitVblank", "Drm64CrtcSeq", "InputEvent64",
]


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)     # /* ... */
    text = re.sub(r"//[^\n]*", " ", text)                  # // ...
    return text


def parse_header(path):
    """返回 (#define 单行表, {结构体名: [规范化字段行]})。多行宏（带续行）跳过。"""
    raw = open(path, "r", encoding="utf-8", errors="replace").read()
    text = _strip_comments(raw)
    defines = {}
    for line in text.splitlines():
        s = line.strip()
        m = re.match(r"#define\s+(\w+)\s+(.*)$", s)
        if not m:
            continue
        name, val = m.group(1), m.group(2).strip()
        if val.endswith("\\"):
            continue                                       # 多行宏单独看
        defines[name] = re.sub(r"\s+", " ", val)
    structs = {}
    for nm in STRUCT_NAMES:
        m = re.search(r"struct\s+(?:__attribute__\(\(packed\)\)\s*)?%s\s*\{(.*?)\};" % nm,
                      text, flags=re.S)
        if not m:
            structs[nm] = None
            continue
        body = []
        for ln in m.group(1).splitlines():
            t = re.sub(r"\s+", " ", ln.strip())
            if not t:
                continue
            t = re.sub(r"\bstruct\s+", "", t)              # 用户侧写 struct Drm64ModeMode，内核侧不写
            body.append(t)
        structs[nm] = body
    return defines, structs


def check_headers(problems):
    k_def, k_st = parse_header(os.path.join(ROOT, "kernel", "drm64.h"))
    u_def, u_st = parse_header(os.path.join(ROOT, "user", "lib", "drm64.h"))
    # ① #define：内核侧每一个都必须出现在用户侧、且值逐字符相同
    for name, val in sorted(k_def.items()):
        if name not in u_def:
            problems.append("用户侧缺 #define %s（内核 = %s）" % (name, val))
        elif u_def[name] != val:
            problems.append("#define %s 不一致：内核 %r / 用户 %r" % (name, val, u_def[name]))
    # ② 结构体：字段序列必须一致
    for nm in STRUCT_NAMES:
        if k_st[nm] is None:
            problems.append("内核侧找不到结构体 %s" % nm)
            continue
        if u_st[nm] is None:
            problems.append("用户侧找不到结构体 %s" % nm)
            continue
        if k_st[nm] != u_st[nm]:
            problems.append("结构体 %s 字段不一致：内核 %s / 用户 %s"
                            % (nm, k_st[nm], u_st[nm]))
    # ③ 关键常量抽查（防止两边同时被改错）
    want = {"DRM64_NR_VERSION": "0x00u", "DRM64_NR_MODE_CREATE_DUMB": "0xB2u",
            "DRM64_MAX_DUMB64": "4u", "DRM64_MAX_CRTC64": "1u", "DRM64_MAX_PLANE64": "2u",
            "DRM64_MAX_CONNECTOR64": "4u"}
    for k, v in want.items():
        if k_def.get(k) != v:
            problems.append("内核侧 %s = %r（期望 %r）" % (k, k_def.get(k), v))
    return len(k_def), len(STRUCT_NAMES)


# ---------------------------------------------------------------------------
# 夹具
# ---------------------------------------------------------------------------
def build_fixture(tmp):
    """sysdisk.img 副本 + 卷里写 /bin/drmdemo.elf + /etc/drm64_probe。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(DISK, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:(PART_MAIN_LBA + TARGET_SECTORS) * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = int.from_bytes(vol[20:24], "little")
    ino_start = int.from_bytes(vol[36:40], "little")
    ed = TP.VolumeEdit(total)
    ed.load(vol)
    # 卷根 0777（登录会话是 uid=1000）：与 pipe64_test 同一处理，只影响这份测试副本
    off = ino_start * SECTOR + 79
    struct.pack_into("<H", ed.buf, off, 0o40777)
    struct.pack_into("<I", ed.buf, ino_start * SECTOR + 124,
                     zlib.crc32(bytes(ed.buf[ino_start * SECTOR:ino_start * SECTOR + 124])) & 0xFFFFFFFF)
    elf = open(DRMDEMO_ELF, "rb").read()
    if len(elf) < 64 or elf[0:4] != b"\x7fELF":
        return None
    ed.put("/bin/drmdemo.elf", elf, mode=0o755)
    ed.put("/etc/drm64_probe", b"1\n", mode=0o644)     # opt-in：跑 ring3 验收程序
    out = ed.finish()
    system = open(os.path.join(BUILD, "system.img"), "rb").read()
    disk = TP.SV.build_disk(system, out, TARGET_SECTORS)
    path = os.path.join(tmp, "drm64.img")
    with open(path, "wb") as f:
        f.write(disk)
    return path


# ---------------------------------------------------------------------------
# QEMU 壳
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM")
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def nonblack_frac(body, w, rect, step=4):
    x, y, rw, rh = rect
    nz = tot = 0
    for yy in range(y, y + rh, step):
        for xx in range(x, x + rw, step):
            tot += 1
            o = (yy * w + xx) * 3
            if body[o:o + 3] != b"\x00\x00\x00":
                nz += 1
    return (nz / tot) if tot else 0.0


class Vm:
    def __init__(self, qemu, img, tmp, timeout):
        self.serial = os.path.join(tmp, "drm64_serial.log")
        self.port = qh.free_port()
        argv = [qemu, "-name", "Vimtu64-drm64",
                "-drive", "format=raw,file=%s" % q(img),
                "-boot", "order=c", "-m", "512", "-vga", "std",
                "-display", "none",
                "-serial", "file:%s" % q(self.serial),
                "-monitor", "telnet:127.0.0.1:%d,server,nowait" % self.port,
                "-no-reboot"]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = None
        for _ in range(80):
            if self.proc.poll() is not None:
                break
            try:
                import socket
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                self.mon = qh.Monitor(self.port)
                break
            except OSError:
                time.sleep(0.25)
        self.timeout = timeout

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(self, needle, timeout=60, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def shot(self, tmp, tag="desktop", wait=2.0):
        path = os.path.join(tmp, "drm64_%s.ppm" % tag)
        if os.path.exists(path):
            os.remove(path)
        if not self.mon or not self.mon.send("screendump %s" % q(path), wait=wait):
            return None
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                try:
                    return read_ppm(path)
                except ValueError:
                    return None
            time.sleep(0.1)
        return None

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--img", default=DISK)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-62s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    print("=== Vimtu64 drm64 acceptance（DRM/KMS + evdev + 混合呈现）===")

    # ---------- ① 头文件逐值比对（不需要 QEMU）----------
    problems = []
    ndef, nst = check_headers(problems)
    check("① 两份 drm64.h 逐值一致（%d 个 #define + %d 个结构体字段序列）" % (ndef, nst),
          not problems, "；".join(problems[:3]))

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    for p in (args.img, os.path.join(BUILD, "system.img"), DRMDEMO_ELF):
        if not os.path.exists(p):
            print("环境问题：缺少构建产物 %s（先跑 bash build64.sh）" % p)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_drm64_")
    img = build_fixture(tmp)
    if not img:
        print("环境问题：造夹具盘失败（sysdisk.img 里没有 VimtuFS2 卷 / drmdemo.elf 不是 ELF）")
        return 2

    vm = Vm(qemu, img, tmp, args.timeout)
    ok = True
    try:
        if vm.mon is None:
            check("QEMU monitor 可连（telnet）", False)
            return 1
        # ---------- ② 内核自检 ----------
        got = vm.wait("[DRM] selftest", 150)
        log = vm.log()
        m = re.search(r"\[DRM\] selftest dumb=ok flip=ok vblank=(\d+) mode=(hw|soft) ok=1", log)
        check("② 内核自检 ok=1（dumb/flip/vblank 三段齐全）", m is not None,
              (m.group(0) if m else "没等到 [DRM] selftest ... ok=1"))
        vblank_self = int(m.group(1)) if m else 0
        check("② 自检的 vblank 序列号 >= 1（真的等过一次）", vblank_self >= 1, "vblank=%d" % vblank_self)
        check("② 自检的内容核对通过（[DRM] verify bars=8 ok=1 —— 内核读回的像素 == 写进去的图案）",
              "[DRM] verify bars=8 ok=1" in log, "")

        # ---------- ③ 混合呈现 / vblank ----------
        m = re.search(r"\[DRM\] init crtc=1 connectors=1 fbs=\d+ dumb_max=4 vblank=(hw|soft)", log)
        check("③ [DRM] init 行字段齐全（crtc=1 connectors=1 dumb_max=4 vblank=hw|soft）", m is not None,
              (m.group(0) if m else "缺 init 行"))
        mm = re.search(r"\[DRM\] vblank mode=(hw|soft) edges=(\d+) ms=(\d+)", log)
        check("③ 垂直回扫探测有实测值（edges/ms 字段齐全）", mm is not None,
              (mm.group(0) if mm else "缺 vblank 行"))
        presents = re.findall(r"\[DRM\] present id=(\d+) path=(gpu|cpu) bytes=(\d+) seq=(\d+)", log)
        # ★ 判据只管"有且字段合法 + path 取值合法"，**不把次数当判据**：
        #   次数取决于内核自检与 ring3 演示各发几次翻转，那是实现细节不是契约。
        check("③ 有 [DRM] present 打点（path=gpu|cpu）", len(presents) >= 1,
              "共 %d 条：%s" % (len(presents), [p[1] for p in presents][:6]))
        check("③ present 的 bytes 与 dumb 尺寸自洽（>0）",
              all(int(p[2]) > 0 for p in presents) if presents else False,
              str([p[2] for p in presents][:4]))
        wait_seqs = [int(x) for x in re.findall(r"\[DRM\] wait_vblank seq=(\d+) mode=(?:hw|soft)", log)]
        # 同上：要求"至少等过一次 + 已观测到的序列号严格递增"，不搞固定次数。
        mono = (len(wait_seqs) >= 1 and
                (all(b > a for a, b in zip(wait_seqs, wait_seqs[1:])) if len(wait_seqs) >= 2 else True))
        check("③ vblank 序列号单调递增（%d 次 wait）" % len(wait_seqs), mono, str(wait_seqs[:8]))

        # ---------- ④ ring3 验收程序 ----------
        stage = [
            (r"\[DRMDEMO\] open fd=\d+", "open fd="),
            (r"\[DRMDEMO\] version major=1 minor=0 patch=0 name=vimtu-drm", "version name=vimtu-drm"),
            (r"\[DRMDEMO\] getresources crtcs=1 connectors=1 fbs=\d+ max=\d+x\d+", "getresources"),
            (r"\[DRMDEMO\] getconnector id=1 connected=1 mode=\d+x\d+", "getconnector connected=1"),
            (r"\[DRMDEMO\] create_dumb handle=\d+ \d+x\d+@32 pitch=\d+ size=\d+", "create_dumb"),
            (r"\[DRMDEMO\] map_dumb handle=\d+ va=0x[0-9a-f]+", "map_dumb va="),
            (r"\[DRMDEMO\] paint bars=8 barw=64 va=0x[0-9a-f]+ probe=0x[0-9a-f]+", "paint bars=8"),
            (r"\[DRMDEMO\] page_flip handle=\d+ rc=0", "page_flip rc=0"),
            (r"\[DRMDEMO\] wait_vblank seq=\d+ hw=[01] rc=0", "wait_vblank"),
        ]
        stage_ok = vm.wait("[DRMDEMO] done", args.timeout)
        log = vm.log()
        for rx, what in stage:
            check("④ [DRMDEMO] %s" % what, re.search(rx, log) is not None)
        check("④ 未知 ioctl 返回 -ENOTTY：badcmd rc=-25（反例，不假装成功）",
              re.search(r"\[DRMDEMO\] badcmd rc=-25", log) is not None,
              (re.search(r"\[DRMDEMO\] badcmd rc=-?\d+", log).group(0)
               if re.search(r"\[DRMDEMO\] badcmd rc=-?\d+", log) else "缺 badcmd 行"))
        check("④ ring3 能写那块 dumb buffer：paint probe=0xffffffff（读回自己写的白色条）",
              re.search(r"\[DRMDEMO\] paint bars=8 barw=64 va=0x[0-9a-f]+ probe=0xffffffff", log) is not None,
              (re.search(r"paint[^\r\n]*probe=0x[0-9a-f]+", log).group(0)
               if re.search(r"paint[^\r\n]*probe=0x[0-9a-f]+", log) else ""))
        check("④ 内核把这块缓冲挂进了用户页表：map ... u=1",
              re.search(r"\[DRM\] map handle=\d+ va=0x[0-9a-f]+ pages=\d+ phys=0x[0-9a-f]+ u=1", log) is not None)
        check("④ evdev 打开成功（[DRMDEMO] evdev open ... name=vimtu-keyboard）",
              re.search(r"\[DRMDEMO\] evdev open fd=\d+ name=vimtu-keyboard", log) is not None,
              (re.search(r"\[DRMDEMO\] evdev[^\r\n]*", log).group(0)
               if re.search(r"\[DRMDEMO\] evdev[^\r\n]*", log) else "缺 evdev 行"))
        check("④ evdev read 有结论（读到事件 或 如实 skipped）",
              re.search(r"\[DRMDEMO\] evdev read (n=\d+|skipped rc=-?\d+)", log) is not None)
        check("④ [DRMDEMO] destroy_dumb rc=0", re.search(r"\[DRMDEMO\] destroy_dumb handle=\d+ rc=0", log) is not None)
        check("④ 验收程序跑完：[DRMDEMO] done ok=1", "[DRMDEMO] done ok=1" in log, "")

        # ---------- ⑤ 桌面照旧 + 屏幕取证 ----------
        try:
            qh.login_desktop(vm.mon, lambda: vm.log(), proc=vm.proc, timeout=min(args.timeout, 200))
        except Exception as e:                                   # noqa: BLE001
            print("  [info] login_desktop 抛异常：%r" % (e,))
        ready = vm.wait("[GUI64] ready", 90)
        check("⑤ 桌面照旧起来（[GUI64] ready）", ready, "")
        shot = vm.shot(tmp, "desktop", wait=2.0)
        if check("⑤ screendump 可用", shot is not None, "" if shot else "screendump 失败"):
            w, h, body = shot
            frac = nonblack_frac(body, w, (0, 0, w, min(h, 200)))
            check("⑤ 屏幕上部非黑像素比例 > 0.3（实测 %.3f）" % frac, frac > 0.3,
                  "%dx%d" % (w, h))
        # ---------- ⑥ 禁止项 ----------
        final = vm.log()
        print("--- 禁止项 ---")
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in final)
    finally:
        vm.close()
        if args.keep:
            print("[drm64] 串口日志：%s" % vm.serial)

    npass = sum(1 for _, c, _ in checks if c)
    nfail = len(checks) - npass
    print("\n==== drm64_test：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, nfail))
    if nfail:
        print("失败项：")
        for name, c, detail in checks:
            if not c:
                print("  [FAIL] %s   %s" % (name, detail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
