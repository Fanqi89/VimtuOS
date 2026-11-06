#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/drm64_test.py - ★ v0.9.12：DRM/KMS + evdev 内核门面 + ring3 验收程序 + 混合呈现

要证明的事（每条都有串口/宿主侧字节证据）：
  ① **两份头文件逐值一致**：kernel/drm64.h 与 user/lib/drm64.h 的 `#define` 数值 +
     每个结构体的字段序列完全相同（ABI 只有一个真源，拷贝必须同步）。
  ①b **ioctl 号与 Linux `include/uapi/drm/drm.h` 逐个相等**（v0.9.14 新增的判据）：
     11 个号 = `_IOWR('d', nr, sizeof(struct))`；sizeof 必须是 Linux 的 64/16/64/104/80/32/
     16/4/24/24/24 —— 三重核对（#define 的 nr、两个头文件的 sizeof 断言、算出来的号 vs 字面值）。
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
    # ★ v0.9.15：fb 对象 / plane
    "Drm64ModeFbCmd2", "Drm64ModeFbCmd", "Drm64ModeGetPlaneRes", "Drm64ModeGetPlane",
    # ★ v0.9.16：属性系统 / blob / atomic
    "Drm64ModeGetEncoder", "Drm64ModeGetProperty", "Drm64ModeGetBlob",
    "Drm64ModeObjGetProps", "Drm64ModeObjSetProp", "Drm64ModeAtomic",
    "Drm64ModeCreateBlob", "Drm64ModeDestroyBlob",
    # ★ v0.9.17：2D 加速（脏矩形）
    "Drm64ModeFbDirty", "Drm64ClipRect",
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


# Linux `include/uapi/drm/drm.h` 的参考值（nr 宏名, 结构体名, nr, sizeof(struct), ioctl 号）
# ioctl 号 = _IOWR('d', nr, size) = (3<<30) | (size<<16) | (0x64<<8) | nr
LINUX_DRM_ABI = [
    ("DRM64_NR_VERSION",           "Drm64Version",        0x00,  64, 0xC0406400),
    ("DRM64_NR_GET_CAP",           "Drm64GetCap",         0x0C,  16, 0xC010640C),
    ("DRM64_NR_MODE_GETRESOURCES", "Drm64ModeResources",  0xA0,  64, 0xC04064A0),
    ("DRM64_NR_MODE_GETCRTC",      "Drm64ModeCrtc",       0xA1, 104, 0xC06864A1),
    ("DRM64_NR_MODE_SETCRTC",      "Drm64ModeCrtc",       0xA2, 104, 0xC06864A2),
    ("DRM64_NR_MODE_GETCONNECTOR", "Drm64ModeConnector",  0xA7,  80, 0xC05064A7),
    ("DRM64_NR_MODE_ADDFB",        "Drm64ModeFbCmd",      0xAD,  28, 0xC01C64AD),
    ("DRM64_NR_MODE_ADDFB2",       "Drm64ModeFbCmd2",     0xAE, 104, 0xC06864AE),
    ("DRM64_NR_MODE_CREATE_DUMB",  "Drm64ModeCreateDumb", 0xB2,  32, 0xC02064B2),
    ("DRM64_NR_MODE_MAP_DUMB",     "Drm64ModeMapDumb",    0xB3,  16, 0xC01064B3),
    ("DRM64_NR_MODE_DESTROY_DUMB", "Drm64ModeDestroyDumb", 0xB4,  4, 0xC00464B4),
    ("DRM64_NR_MODE_PAGE_FLIP",    "Drm64ModePageFlip",   0xB0,  24, 0xC01864B0),
    ("DRM64_NR_MODE_GETPLANE",     "Drm64ModeGetPlane",   0xB5,  32, 0xC02064B5),
    ("DRM64_NR_MODE_GETPLANERES",  "Drm64ModeGetPlaneRes", 0xB6, 16, 0xC01064B6),
    ("DRM64_NR_WAIT_VBLANK",       "Drm64WaitVblank",     0x3A,  24, 0xC018643A),
    ("DRM64_NR_CRTC_GET_SEQUENCE", "Drm64CrtcSeq",        0x3B,  24, 0xC018643B),
    # ★ v0.9.16：属性系统 / blob / atomic（8 个）
    ("DRM64_NR_MODE_GETENCODER",   "Drm64ModeGetEncoder",  0xA6,  20, 0xC01464A6),
    ("DRM64_NR_MODE_GETPROPERTY",  "Drm64ModeGetProperty", 0xAA,  64, 0xC04064AA),
    ("DRM64_NR_MODE_GETPROPBLOB",  "Drm64ModeGetBlob",     0xAC,  16, 0xC01064AC),
    ("DRM64_NR_MODE_OBJ_GETPROPS", "Drm64ModeObjGetProps", 0xB9,  32, 0xC02064B9),
    ("DRM64_NR_MODE_OBJ_SETPROP",  "Drm64ModeObjSetProp",  0xBA,  24, 0xC01864BA),
    ("DRM64_NR_MODE_ATOMIC",       "Drm64ModeAtomic",      0xBC,  56, 0xC03864BC),
    ("DRM64_NR_MODE_CREATEBLOB",   "Drm64ModeCreateBlob",  0xBD,  16, 0xC01064BD),
    ("DRM64_NR_MODE_DESTROYBLOB",  "Drm64ModeDestroyBlob", 0xBE,   4, 0xC00464BE),
    # ★ v0.9.17：2D 加速（脏矩形）
    ("DRM64_NR_MODE_DIRTYFB",      "Drm64ModeFbDirty",     0xB1,  24, 0xC01864B1),
]


def _sizeof_asserts(path, user_side):
    """读出每个结构体的 sizeof 断言值：这是 ABI 的第二道锁（第一道是字段序列）。"""
    text = _strip_comments(open(path, "r", encoding="utf-8", errors="replace").read())
    if user_side:
        pat = r"DRM64_STATIC_ASSERT\(\s*sizeof\(\s*(?:struct\s+)?(\w+)\s*\)\s*==\s*(\d+)"
    else:
        pat = r"static_assert\(\s*sizeof\(\s*(?:struct\s+)?(\w+)\s*\)\s*==\s*(\d+)"
    return {m.group(1): int(m.group(2)) for m in re.finditer(pat, text)}


def check_linux_abi(problems):
    """★ 与 Linux drm.h 的判据（v0.9.14 新增，只许加强）：
       ioctl 号的 size 位就是 sizeof(struct)，所以 sizeof 与 Linux 相等 ⇒ DRM64_IOWR(nr, type)
       算出来的号必然等于 Linux 的 _IOWR('d', nr, size)。这里三重核对：
         a) 头文件 #define 的 nr 值
         b) 两份头文件里 sizeof 断言的数值（内核 static_assert / 用户 DRM64_STATIC_ASSERT）
         c) 用 (3<<30)|(sizeof<<16)|('d'<<8)|nr 算出来的号 vs Linux 字面值
    """
    KP = os.path.join(ROOT, "kernel", "drm64.h")
    UP = os.path.join(ROOT, "user", "lib", "drm64.h")
    k_def, _ = parse_header(KP)
    k_sz = _sizeof_asserts(KP, False)
    u_sz = _sizeof_asserts(UP, True)
    n = 0
    for nrname, stname, nr, size, num in LINUX_DRM_ABI:
        n += 1
        v = k_def.get(nrname)
        if v is None:
            problems.append("内核侧缺 #define %s" % nrname)
        else:
            try:
                got_nr = int(v.rstrip("uU"), 16) & 0xFF
            except ValueError:
                problems.append("%s 的值 %r 不是十六进制" % (nrname, v))
                got_nr = -1
            if got_nr != nr:
                problems.append("%s = %s（Linux nr = 0x%02x）" % (nrname, v, nr))
        for side, tbl in (("内核", k_sz), ("用户", u_sz)):
            got = tbl.get(stname)
            if got is None:
                problems.append("%s侧没有 sizeof(%s) 的断言" % (side, stname))
            elif got != size:
                problems.append("%s侧 sizeof(%s) = %d（Linux = %d）" % (side, stname, got, size))
        calc = (3 << 30) | (size << 16) | (0x64 << 8) | nr
        if calc != num:
            problems.append("ioctl(%s) 的 Linux 参考值算错：0x%08X / 期望 0x%08X" % (nrname, calc, num))
        msz = k_sz.get(stname)
        if msz is not None and ((3 << 30) | (msz << 16) | (0x64 << 8) | nr) != num:
            problems.append("DRM64_IOWR(%s, %s) = 0x%08X（Linux = 0x%08X）"
                            % (nrname, stname, (3 << 30) | (msz << 16) | (0x64 << 8) | nr, num))
    # ioctl 表之外、但 Linux 也有同名布局的：evdev 的 struct input_event
    for nm, size in (("InputEvent64", 24),):
        for side, tbl in (("内核", k_sz), ("用户", u_sz)):
            if tbl.get(nm) != size:
                problems.append("%s侧 sizeof(%s) = %r（Linux = %d）" % (side, nm, tbl.get(nm), size))
    return n


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

    abi_problems = []
    nabi = check_linux_abi(abi_problems)
    check("①b %d 个 ioctl 号与 Linux drm.h 逐值相等（sizeof 逐字节相同，含 InputEvent64）" % nabi,
          not abi_problems, "；".join(abi_problems[:4]))

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
            (r"\[DRMDEMO\] crtc_seq active=[01] seq=\d+ ns=\d+ rc=0", "crtc_seq active/seq/ns"),
            (r"\[DRMDEMO\] paint bars=8 barw=64 va=0x[0-9a-f]+ probe=0x[0-9a-f]+", "paint bars=8"),
            (r"\[DRMDEMO\] page_flip handle=\d+ rc=0", "page_flip rc=0"),
            (r"\[DRMDEMO\] wait_vblank seq=\d+ hw=[01] rc=0", "wait_vblank"),
            (r"\[DRMDEMO\] plane_res n=2 ids=1,2 rc=0", "plane_res n=2 ids=1,2"),
            (r"\[DRMDEMO\] plane id=1 crtc=1 fb=\d+ formats=2 fmt0=0x34325258 rc=0", "plane fmt0=XRGB8888"),
            (r"\[DRMDEMO\] addfb2 fb=\d+ handle=\d+ rc=0", "addfb2 rc=0"),
            (r"\[DRMDEMO\] setcrtc crtc=1 fb=\d+ rc=0", "setcrtc rc=0"),
            (r"\[DRMDEMO\] leak_dumb handle=\d+ pages_hint=4 rc=0", "leak_dumb（故意不销毁）"),
            (r"\[DRMDEMO\] encoder id=1 type=2 crtc=1 possible_crtcs=0x1 rc=0", "encoder"),
            (r"\[DRMDEMO\] obj_props plane n=12 counted=12 first=3 fbval=\d+ rc=0", "obj_props n=12"),
            (r"\[DRMDEMO\] prop id=4 name=FB_ID flags=0x12 rc=0", "prop FB_ID flags=0x12"),
            (r"\[DRMDEMO\] blob_informats len=56 version=1 formats=2 mods=1 fmt0=0x34325241 rc=0",
             "IN_FORMATS blob（version/formats/mods）"),
            (r"\[DRMDEMO\] blob_mode id=768 len=68 rc=0", "CREATEBLOB mode 68 B -> id 768"),
            (r"\[DRMDEMO\] atomic badscale rc=-22", "atomic 反例：缩放 -> -22"),
            (r"\[DRMDEMO\] atomic badevent rc=-22", "atomic 反例：PAGE_FLIP_EVENT -> -22"),
            (r"\[DRMDEMO\] atomic test_only rc=0", "atomic TEST_ONLY rc=0"),
            (r"\[DRMDEMO\] atomic commit rc=0 user_data=0x5150", "atomic commit rc=0"),
            (r"\[DRMDEMO\] after_atomic plane_fb=256 crtc_fb=256 valid=1 rc=0/0",
             "提交后 GETPLANE/GETCRTC 报同一个 fb"),
            (r"\[DRMDEMO\] blob_destroy id=768 rc=0", "DESTROYBLOB rc=0"),
            (r"\[DRMDEMO\] dirtyfb badflags rc=-22", "DIRTYFB 反例（flags）-> -22"),
            (r"\[DRMDEMO\] dirtyfb handle=\d+ clips=1 rc=0", "DIRTYFB rc=0"),
            (r"\[DRMDEMO\] flip_b rc=0", "双缓冲 flip_b"),
            (r"\[DRMDEMO\] flip_a rc=0", "双缓冲 flip_a"),
            (r"\[DRMDEMO\] dumb_b handle=\d+ rc=0/0", "第二块 dumb（双缓冲的 B 块）"),
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

        # ---------- ④-b ★ v0.9.15：fb 对象 / SETCRTC / plane / 进程回收 hook ----------
        check("④ [DRM] addfb2 内核侧建 fb 成功（fmt=xrgb8888，fb id 从 0x100 起）",
              re.search(r"\[DRM\] addfb2 fb=\d+ handle=\d+ w=\d+ h=\d+ pitch=\d+ fmt=xrgb8888", log) is not None,
              (re.search(r"\[DRM\] addfb2[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] addfb2[^\r\n]*", log) else "缺内核侧 addfb2 行"))
        check("④ 反例：ADDFB2 的 NV12 被如实拒绝 -> [DRMDEMO] addfb2 badfmt rc=-22",
              "[DRMDEMO] addfb2 badfmt rc=-22" in log,
              (re.search(r"\[DRMDEMO\] addfb2 badfmt[^\r\n]*", log).group(0)
               if re.search(r"\[DRMDEMO\] addfb2 badfmt[^\r\n]*", log) else "缺 badfmt 行"))
        check("④ [DRM] setcrtc 用 fb id 上屏（mode=ok + path=gpu|cpu）",
              re.search(r"\[DRM\] setcrtc crtc=1 fb=\d+ x=0 y=0 mode=ok path=(gpu|cpu)", log) is not None,
              (re.search(r"\[DRM\] setcrtc[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] setcrtc[^\r\n]*", log) else "缺 setcrtc 行"))
        check("④ [DRM] plane 报告 primary 正在扫出的 fb（formats=2）",
              re.search(r"\[DRM\] plane id=1 crtc=1 fb=\d+ formats=2", log) is not None)
        rel = re.search(r"\[DRM\] release pid=(\d+) fbs=(\d+) pages=(\d+) free_pages=(\d+)", log)
        check("④ 进程回收 hook 真收走了没 DESTROY 的那块 dumb（[DRM] release … fbs>=1 pages>=1）",
              rel is not None and int(rel.group(2)) >= 1 and int(rel.group(3)) >= 1,
              (rel.group(0) if rel else "没等到 [DRM] release —— hook 没跑"))
        if rel is not None:
            i_done = log.find("[DRMDEMO] done ok=1")
            i_rel = log.find(rel.group(0))
            check("④ release 发生在 [DRMDEMO] done **之后**（= 退出 hook，不是显式 destroy）",
                  i_done >= 0 and i_rel > i_done, "done@%d release@%d" % (i_done, i_rel))
        else:
            check("④ release 发生在 [DRMDEMO] done **之后**（= 退出 hook，不是显式 destroy）", False, "缺 release 行")

        # ---------- ④-c ★ v0.9.16：属性系统 + atomic commit ----------
        check("④ [DRM] encoder 报 TMDS + possible_crtcs=0x1",
              re.search(r"\[DRM\] encoder id=1 type=2 crtc=\d+ possible_crtcs=0x1", log) is not None,
              (re.search(r"\[DRM\] encoder[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] encoder[^\r\n]*", log) else "缺 encoder 行"))
        check("④ [DRM] prop get 把名字/标志回给用户态（FB_ID, flags=0x12）",
              re.search(r"\[DRM\] prop get id=4 name=FB_ID flags=0x12", log) is not None)
        check("④ IN_FORMATS blob 可读回（56 B，version=1 / 2 个格式 / 1 条 linear modifier）",
              re.search(r"\[DRM\] blob read id=513 len=56", log) is not None,
              (re.search(r"\[DRM\] blob read[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] blob read[^\r\n]*", log) else "缺 blob read 行"))
        check("④ CREATEBLOB 把模式做成 MODE_ID blob（68 B -> id 768）",
              re.search(r"\[DRM\] blob create id=768 len=68", log) is not None)
        check("④ 反例：atomic 不支持缩放 -> [DRM] atomic FAILED reason=no-scaling err=22",
              re.search(r"\[DRM\] atomic FAILED reason=no-scaling err=22", log) is not None)
        check("④ 反例：atomic 的 PAGE_FLIP_EVENT 位被拒 -> reason=flags err=22",
              re.search(r"\[DRM\] atomic FAILED reason=flags err=22", log) is not None)
        check("④ TEST_ONLY 只校验不落像素（[DRM] atomic … test=1 path=none）",
              re.search(r"\[DRM\] atomic objs=3 props=10 flags=0x500 test=1 path=none", log) is not None,
              (re.search(r"\[DRM\] atomic[^\r\n]*test=1[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] atomic[^\r\n]*test=1[^\r\n]*", log) else "缺 test=1 行"))
        check("④ 真提交走通（[DRM] atomic … test=0 path=cpu active=1）",
              re.search(r"\[DRM\] atomic objs=3 props=10 flags=0x400 test=0 path=(gpu|cpu) active=1", log) is not None,
              (re.search(r"\[DRM\] atomic[^\r\n]*test=0[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] atomic[^\r\n]*test=0[^\r\n]*", log) else "缺 test=0 行"))
        # 顺序证据：TEST_ONLY 自己**不落像素**。判据用"present 计数差"：
        #   从 test=1 那行到 commit 那行之间，presnet 只该多 1 次（commit 自己那次）。
        #   如果 TEST_ONLY 也上屏了，差就是 2 —— 那样就红。
        i_t1 = log.find("test=1 path=none")
        i_t0 = log.find("test=0 path=")
        if i_t1 >= 0 and i_t0 > i_t1:
            n_before = log[:i_t1].count("[DRM] present")
            n_after = log[:i_t0].count("[DRM] present")
            check("④ TEST_ONLY 没落像素（它之后到真提交之间只多 1 次 present = 提交自己那次）",
                  n_after - n_before == 1, "before=%d after=%d" % (n_before, n_after))
        else:
            check("④ TEST_ONLY 没落像素（它之后到真提交之间只多 1 次 present = 提交自己那次）", False,
                  "找不到 test=1/test=0 两行")

        # ---------- ④-d ★ v0.9.17：2D 加速（脏矩形只搬该搬的）----------
        check("④ [DRM] dirty 记下脏矩形（clips=1 dmg=0,0,128,64 bytes=32768）",
              re.search(r"\[DRM\] dirty fb=\d+ clips=1 dmg=0,0,128,64 bytes=32768", log) is not None,
              (re.search(r"\[DRM\] dirty[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] dirty[^\r\n]*", log) else "缺 dirty 行"))
        pres = re.findall(r"\[DRM\] present id=(\d+) path=(gpu|cpu) bytes=(\d+)", log)
        small = [int(b) for _i, _p, b in pres if int(b) == 32768]
        check("④ 脏矩形那次提交只搬 32 KiB（整屏是 3,145,728 B —— 2D 加速的实际收益）",
              len(small) >= 1, "各次 bytes=%s" % [int(b) for _i, _p, b in pres][:8])
        check("④ 双缓冲交替被内核记录下来（[DRM] flip … alt=1 出现 2 次）",
              len(re.findall(r"\[DRM\] flip a=\d+ b=\d+ alt=1", log)) >= 2,
              (re.search(r"\[DRM\] flip[^\r\n]*", log).group(0)
               if re.search(r"\[DRM\] flip[^\r\n]*", log) else "缺 flip 行"))
        check("④ 反例：DIRTYFB 的 flags 非 0 -> [DRM] dirty FAILED reason=flags err=22",
              re.search(r"\[DRM\] dirty FAILED reason=flags err=22", log) is not None)

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
