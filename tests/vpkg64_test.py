#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/vpkg64_test.py - ★ 应用商店 + 包管理器（用户计划 ⑩）端到端验收
                      —— CLI / .deb / GUI 三路 + 三条反例 + **关 QEMU 后宿主侧解析系统卷**

被验的东西（全部用户态：/bin/vpkg、/bin/store、/opt/vpkg/repo、/var/lib/vpkg/installed.json）：
  ① **CLI 路**：ring3 外壳里 `run /bin/vpkg list|info|install|remove|search|update`；
     装完的包**真能跑**（`run /bin/hello-cli` 的 stdout 逐字节对上 SDK 模板的串）；
  ② **deb 路**：宿主造真 `.deb`（ar + control.tar.gz + data.tar.gz）装成功；带 preinst 的
     **被如实拒绝**（"maintainer scripts not supported"，有界、无 PANIC、盘上零残留）；
     xz 压缩的**被如实拒绝**（VS_E_XZ）；
  ③ **GUI 路**：`/bin/store` 窗口（/lib/wm.elf 合成器 + font64 画字）-> 列表渲染（**像素/墨迹断言**）
     -> 点选行 -> 点「安装」-> 进度/完成 -> 点「启动」-> 装出来的 GUI 包**真跑起来**（串口打点）；
  ④ **反例**：坏 sha256 / 缺依赖 / 重复安装 各一条断言（都返回各自的错误码，盘上不留东西）；
  ⑤ **关 QEMU 后宿主侧**：卷里文件逐字节存在（.vap64 的 payload、deb 的 data.tar 内容、包本体副本）、
     `/var/lib/vpkg/installed.json` 记录正确、**卸载后文件消失**、被拒的包零残留。

夹具（宿主侧，本脚本自己造）：
  * `build64/store/repo_test/`：用 tools/store_pack_win.py 的 build_repo() 打两个 .vap64
    （**SDK 的 vap64_pack**，一个 CLI = SDK hello-cli.elf、一个 GUI = build64/store/demo_gui.elf）
    + 三个 .deb（纯 payload / 带 preinst / xz）+ 两条反例条目（坏 sha256、装不下的 2 MiB payload）
    + `index.json`（含 depends 关系：gui-demo 依赖 hello-cli）；
  * 夹具盘：`--fresh-vol 3200`（1.6 MB 卷）—— 故意做小，让"空间不足"这条反例**真能触发**。

用法（Windows 原生 Python；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\vpkg64_test.py [--qemu PATH] [--keep] [--no-gui]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU / 构建产物缺失）。
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh                                            # noqa: E402（公共登录手势）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
STORE_DIR = os.path.join(ROOT, "build64", "store")
VPKG_ELF = os.path.join(STORE_DIR, "vpkg.elf")
STORE_ELF = os.path.join(STORE_DIR, "store.elf")
DEMO_GUI_ELF = os.path.join(STORE_DIR, "demo_gui.elf")
DEMO_CLI_ELF = os.path.join(STORE_DIR, "demo_cli.elf")
SDK_HELLO_CLI = os.path.join(ROOT, "build64", "sdk", "hello-cli.elf")
WM_ELF = os.path.join(ROOT, "build64", "wm.elf")
FONT_LATIN = os.path.join(ROOT, "build", "font_bahnschrift.ttf")

FIXTURE_IMG = os.path.join(ROOT, "build64", "store64_test.img")
FIXTURE_REPO = os.path.join(ROOT, "build64", "store", "repo_test")
FRESH_VOL_SECTORS = 3200                       # 1.6 MB 卷（故意小：让"空间不足"能触发）
PART_MAIN_LBA = 8009

# ---- store.c 的布局常量的**镜像**（改 store.c 时这里要一起改；测试按它算点击坐标）----
STORE_W, STORE_H = 256, 60
ROW0_Y, ROW_H = 14, 12
BTN_Y, BTN_H = 48, 12
BTN_INSTALL_X, BTN_INSTALL_W = 4, 60
BTN_LAUNCH_X, BTN_LAUNCH_W = 132, 58
CO_OK = (0x3F, 0xB5, 0x5F)                    # store.c 的 CO_OK（"装了"的绿图标/进度条）
CO_ROWSEL = (0x2E, 0x4A, 0x6C)                # store.c 的 CO_ROWSEL（选中行底色）

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
          "=": "equal", ">": "shift-dot", "_": "shift-minus"}

PANIC_MARKERS = ["PANIC", "TRIPLE FAULT", "FAILED mask="]

# SDK 模板 hello-cli 的输出串（tests/vpkg64_test 的 ① 断言用它；真源 = SDK 的 main.c）
HELLO_CLI_OUT = "[HELLO-CLI] hello from an installed package? "      # 占位（见 prepare_fixture 里替换）
HELLO_CLI_MARK = "[HELLO-CLI] hello"
HELLO_CLI_SUMMARY = "rc=0"

MAINTAINER_MSG = "maintainer scripts not supported"


# ---------------------------------------------------------------------------
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
            found = shutil.which(c)
            if found:
                return found
    return None


def load_tools():
    """tools/store_pack_win.py + tools/tcc_pack_win.py（夹具造包/读卷都走它们）。"""
    out = {}
    for name, fname in (("spw", "store_pack_win.py"), ("tp", "tcc_pack_win.py")):
        path = os.path.join(ROOT, "tools", fname)
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        out[name] = mod
    return out["spw"], out["tp"]


# ---------------------------------------------------------------------------
# QEMU + monitor（与仓库其它 tests/*.py 同款）
# ---------------------------------------------------------------------------
class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def move(self, dx, dy, wait=0.05):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def click(self, btn=1, wait=0.45):
        self.send("mouse_button %d" % btn, wait=0.10)
        self.send("mouse_button 0", wait=wait)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n = os.path.getsize(ppm)
                time.sleep(0.12)
                if os.path.getsize(ppm) == n:
                    return True
            time.sleep(0.05)
        return False


class Vm:
    def __init__(self, qemu, img, port, serial, name, timeout=180):
        self.serial = serial
        self.timeout = timeout
        if os.path.exists(serial):
            os.remove(serial)
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(160):
            if self.proc.poll() is not None:
                break
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def n(self):
        return len(self.log())

    def wait_re(self, rx, timeout=None, since=0):
        timeout = timeout or self.timeout
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log()[since:])
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.2)
        return None

    def wait_mark(self, needle, timeout=None, since=0):
        timeout = timeout or self.timeout
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return needle in self.log()[since:]

    def monitor(self, port):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                return Monitor(port)
            except OSError:
                time.sleep(0.25)
        return None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def type_line(mon, text, per_key=0.10):
    for ch in text:
        if ch in KEYMAP:
            mon.key(KEYMAP[ch], wait=per_key)
        elif ch.isalnum():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.3)


def read_ppm(path):
    d = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while i < len(d) and d[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]


def count_color(px, w, h, want, tol=12, rect=None):
    x0, y0, x1, y1 = rect if rect else (0, 0, w, h)
    n = 0
    for y in range(y0, min(y1, h)):
        base = (y * w) * 3
        for x in range(x0, min(x1, w)):
            o = base + x * 3
            if (abs(px[o] - want[0]) <= tol and abs(px[o + 1] - want[1]) <= tol
                    and abs(px[o + 2] - want[2]) <= tol):
                n += 1
    return n


def count_bright(px, w, h, rect, thresh=120):
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(y0, min(y1, h)):
        base = (y * w) * 3
        for x in range(x0, min(x1, w)):
            o = base + x * 3
            if (px[o] + px[o + 1] + px[o + 2]) // 3 > thresh:
                n += 1
    return n


# ---------------------------------------------------------------------------
# 鼠标：QEMU 相对位移（客人 ×1.7 / 每包 24 px 上限）+ **用 store 自己报的坐标闭环**
# ---------------------------------------------------------------------------
class Pad:
    def __init__(self, mon, vm, start=(512, 384)):
        self.mon = mon
        self.vm = vm
        self.x, self.y = start

    def move_to(self, tx, ty, tol=3, max_pkts=400):
        for _ in range(max_pkts):
            rx, ry = tx - self.x, ty - self.y
            if abs(rx) <= tol and abs(ry) <= tol:
                return True
            cx = max(-14, min(14, int(rx / 1.7)))
            cy = max(-14, min(14, int(ry / 1.7)))
            if cx == 0 and cy == 0:
                return abs(rx) <= tol and abs(ry) <= tol
            self.mon.move(cx, cy)
            self.x = max(0, min(1279, self.x + int(cx * 17 / 10)))
            self.y = max(0, min(799, self.y + int(cy * 17 / 10)))
        return False

    def resync(self, abs_x, abs_y):
        """客人真值：解析 store 打出的 `[STORE] mouse sx=.. sy=..` + surface 屏上坐标。"""
        self.x, self.y = abs_x, abs_y


def last_store_mouse(vm, panel):
    """返回最近一条 [STORE] mouse 的**屏幕绝对坐标**（window 在 panel 原点）。"""
    ms = re.findall(r"\[STORE\] mouse sx=(-?\d+) sy=(-?\d+) x=(-?\d+) y=(-?\d+)", vm.log())
    if not ms:
        return None
    sx, sy, ex, ey = (int(v) for v in ms[-1])
    return (ex, ey)


# ---------------------------------------------------------------------------
# 夹具
# ---------------------------------------------------------------------------
def repo_spec():
    """测试夹具仓库：两个 .vap64 + 三个 .deb + 两条反例条目（size/sha256 由工具算）。"""
    return {"repo": [
        {"name": "hello-cli", "version": "1.0", "type": "vap64", "payload": SDK_HELLO_CLI,
         "file": "hello-cli.vap64", "summary": "SDK hello-cli sample (CLI)",
         "description": "The SDK template CLI sample packed with the SDK vap64_pack",
         "depends": []},
        {"name": "gui-demo", "version": "1.0", "type": "vap64", "payload": DEMO_GUI_ELF,
         "file": "gui-demo.vap64", "summary": "VimtuOS demo GUI app (wm client)",
         "description": "Small GUI package: draws a few frames through the compositor",
         "depends": ["hello-cli"]},
        {"name": "vpkg-debdemo", "version": "1.0", "type": "deb", "file": "vpkg-debdemo.deb",
         "summary": "Pure payload .deb (ar + control.tar.gz + data.tar.gz)",
         "description": "Data-only .deb built on the host", "depends": [],
         "deb": {"control": {"Package": "vpkg-debdemo", "Version": "1.0",
                             "Architecture": "vimtu64",
                             "Maintainer": "VimtuOS <dev@vimtuos.invalid>",
                             "Description": "pure payload demo"},
                 "files": {"usr/share/vpkg-debdemo/data.txt": "vpkg deb fixture: 0123456789\n",
                           "usr/share/vpkg-debdemo/hello.txt": "hello from data.tar.gz\n"}}},
        {"name": "baddeb", "version": "1.0", "type": "deb", "file": "baddeb.deb",
         "summary": ".deb with a preinst maintainer script (must be refused)",
         "description": "Negative case: maintainer scripts are not supported", "depends": [],
         "deb": {"control": {"Package": "baddeb", "Version": "1.0", "Architecture": "vimtu64",
                             "Description": "has preinst"},
                 "files": {"usr/share/baddeb/x.txt": "should never land\n"},
                 "scripts": {"preinst": "#!/bin/sh\necho preinst should not run\n"}}},
        {"name": "xzdeb", "version": "1.0", "type": "deb", "file": "xzdeb.deb",
         "summary": ".deb with xz members (must be refused)",
         "description": "Negative case: xz compression is not supported", "depends": [],
         "deb": {"control": {"Package": "xzdeb", "Version": "1.0", "Architecture": "vimtu64",
                             "Description": "xz members"},
                 "files": {"usr/share/xzdeb/y.txt": "should never land\n"},
                 "compress": "xz"}},
        {"name": "brokenpkg", "version": "1.0", "type": "vap64", "payload": DEMO_CLI_ELF,
         "file": "brokenpkg.vap64", "summary": "index sha256 is wrong on purpose",
         "description": "Negative case: bad hash", "depends": [],
         "sha256_override": "0" * 64},
        {"name": "spacehog", "version": "1.0", "type": "deb", "file": "spacehog.deb",
         "summary": ".deb whose payload cannot fit the small fixture volume",
         "description": "Negative case: not enough space (2 MiB payload)", "depends": [],
         "deb": {"control": {"Package": "spacehog", "Version": "1.0", "Architecture": "vimtu64",
                             "Description": "big payload"},
                 "files": {"usr/share/spacehog/big.bin": {"zeros": 2097152}}}},
    ]}


def prepare_fixture(spw, verbose=True):
    """造仓库 + 夹具盘（走 tools/store_pack_win.py 的**公开路径**，含它自己的逐字节回读自检）。"""
    if not os.path.exists(SDK_HELLO_CLI):
        # 夹具①：SDK 模板的 CLI 示例（打 .vap64 的 payload）。整仓 build64.sh **不**编它
        # （它是 SDK 自己的产物，默认落 build64/sdk/，会被整仓构建清掉），
        # 所以这里按 SDK 的**公开路径**（apps/hello-cli/build.sh）现编一次。
        bash = r"C:\msys64\usr\bin\bash.exe"
        if not os.path.exists(bash):
            raise RuntimeError("缺少 %s，且找不到 MSYS2 bash 去编它" % SDK_HELLO_CLI)
        rel = "sdk/software-template/apps/hello-cli/build.sh"
        r = subprocess.run([bash, "-l", "-c",
                            "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash %s" % rel],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if verbose:
            print((r.stdout or "")[-400:])
        if not os.path.exists(SDK_HELLO_CLI):
            raise RuntimeError("编 SDK hello-cli 失败（rc=%d）：%s"
                               % (r.returncode, (r.stderr or "")[-300:]))
    for p in (SYSTEM_IMG, SHELL_BIN, VPKG_ELF, STORE_ELF, DEMO_GUI_ELF, DEMO_CLI_ELF,
              SDK_HELLO_CLI, WM_ELF, FONT_LATIN):
        if not os.path.exists(p):
            raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % p)
    spec = repo_spec()
    os.makedirs(FIXTURE_REPO, exist_ok=True)
    index = spw.build_repo(spec, FIXTURE_REPO, base=ROOT, verbose=verbose)
    bad = spw.verify_repo(FIXTURE_REPO, index, verbose=verbose)
    if bad:
        raise RuntimeError("夹具仓库自检失败：%s" % bad)

    srcs = [
        (SHELL_BIN, "/bin/shell.bin", 0o755),
        (VPKG_ELF, "/bin/vpkg", 0o755),
        (STORE_ELF, "/bin/store", 0o755),
        (WM_ELF, "/lib/wm.elf", 0o755),
        (FONT_LATIN, "/Fonts/NotoSans-Regular.ttf", 0o644),
    ]
    wmprobe = b"/* wm probe: exists = full mode (Ring 3 compositor) */\n"
    probe_host = os.path.join(FIXTURE_REPO, "_wm_probe")
    with open(probe_host, "wb") as f:
        f.write(wmprobe)
    srcs.append((probe_host, "/etc/wm_probe", 0o644))

    cmd = [sys.executable, os.path.join(ROOT, "tools", "store_pack_win.py"),
           "--fixture-img", FIXTURE_IMG, "--fresh-vol", str(FRESH_VOL_SECTORS),
           "--repo", FIXTURE_REPO, "--system", SYSTEM_IMG, "--writable-roots"]
    for host, vol, mode in srcs:
        cmd += ["--src", "%s:%s:%o" % (host, vol, mode)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if verbose:
        print(r.stdout)
        if r.stderr:
            print(r.stderr)
    if r.returncode != 0:
        raise RuntimeError("造夹具盘失败（rc=%d）" % r.returncode)
    return index


# ---------------------------------------------------------------------------
# 宿主侧卷解析（**关 QEMU 之后**的字节证据）
# ---------------------------------------------------------------------------
def vol_bytes(img):
    with open(img, "rb") as f:
        f.seek(PART_MAIN_LBA * 512)
        return f.read(FRESH_VOL_SECTORS * 512)


def vol_read(tp, vol, path):
    total = struct.unpack_from("<I", vol, 20)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in tp._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return tp._read_file(vol, rec, total)
        cur = ino
    return None


def vap_payload(blob):
    """从 .vap64 里取出 payload（code 段）——与 vpkg 的安装语义一致。"""
    ver, hs, eo, cs, crc, nl = struct.unpack_from("<6I", blob, 8)
    assert blob[0:8] == b"VAP64\0\0\0" and ver == 1 and hs == 32 and eo == 32 + nl
    assert 32 + nl + cs == len(blob)
    return blob[eo:eo + cs]


def deb_data_files(deb):
    """宿主**独立**解一遍 .deb 的 data.tar.gz，返回 {卷内路径: 字节}（不信 vpkg 的自述）。"""
    import tarfile
    import gzip
    import io
    assert deb[:8] == b"!<arch>\n"
    off, members = 8, {}
    while off + 60 <= len(deb):
        h = deb[off:off + 60]
        name = h[0:16].decode("ascii").split("/")[0].strip()
        size = int(h[48:58].decode("ascii").strip() or "0")
        members[name] = deb[off + 60:off + 60 + size]
        off = off + 60 + size + (size & 1)
    out = {}
    data = gzip.decompress(members["data.tar.gz"])
    with tarfile.open(fileobj=io.BytesIO(data)) as tf:
        for ti in tf.getmembers():
            if ti.isfile():
                nm = ti.name[2:] if ti.name.startswith("./") else ti.name
                out[nm] = tf.extractfile(ti).read()
    return out


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5611)
    ap.add_argument("--timeout", type=int, default=200)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-gui", action="store_true", help="只跑 CLI/deb/反例 + 宿主体检（跳过 GUI）")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):     # 控制台可能是 GBK：遇到解不出的字符别炸
        try:
            stream.reconfigure(errors="replace")
        except Exception:                        # noqa: BLE001
            pass


    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    try:
        spw, tp = load_tools()
    except Exception as ex:                                   # noqa: BLE001
        sys.stderr.write("加载 tools/ 失败：%s\n" % ex)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_vpkg_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 应用商店 + 包管理器 acceptance（CLI / deb / GUI + 反例 + 宿主侧卷校验）===")
    try:
        index = prepare_fixture(spw)
    except Exception as ex:                                   # noqa: BLE001
        sys.stderr.write("夹具失败（环境问题）：%s\n" % ex)
        return 2
    by_name = {e["name"]: e for e in index["packages"]}
    repo_host = {e["file"]: open(os.path.join(FIXTURE_REPO, e["file"]), "rb").read()
                 for e in index["packages"]}
    print("[vpkg64] 夹具盘：%s（%d 扇区卷 = %.2f MB）"
          % (FIXTURE_IMG, FRESH_VOL_SECTORS, FRESH_VOL_SECTORS * 512 / 1048576.0))


    monkey = None
    mon = None
    logA = logB = ""
    try:
        # ================= 第一遍：CLI + deb + 反例 =================",
        vm = Vm(qemu, FIXTURE_IMG, args.port, os.path.join(tmp, "boot1.log"), "vimtu-vpkg-cli",
                args.timeout)
        mon = vm.monitor(args.port)
        qh.login_desktop(mon, vm.log, vm.proc, timeout=min(args.timeout, 180))
        up = vm.wait_mark("[GUI64] ready", args.timeout)
        check("桌面就绪 [GUI64] ready", up)
        # 打开终端（开始菜单 -> 数字 1 = 终端），再进 **ring3 外壳**（`run` 才带参数）
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if vm.wait_mark("[APP] term opened", 12):
                opened = True
                break
        check("终端打开（[APP] term opened）", opened)
        type_line(mon, "shell", per_key=0.12)
        sh = vm.wait_re(r"\[SH64\] launch path=/bin/shell.bin size=(\d+) pid=(\d+)", 40)
        check("ring3 外壳 /bin/shell.bin 起来了（[SH64] launch）", sh is not None,
              sh.group(0) if sh else "没等到")
        vm.wait_mark("VimtuOS ring3 shell", 20)

        def run(vm, mon, argv, expect, timeout=45, expect_absent=None):
            """在 ring3 外壳里 `run <argv>`；返回 (是否等到 expect, 这一段日志)。"""
            n0 = vm.n()
            type_line(mon, "run " + argv, per_key=0.09)
            got = vm.wait_mark(expect, timeout, since=n0)
            seg = vm.log()[n0:]
            if expect_absent and expect_absent in seg:
                got = False
            return got, seg

        # ---- ① list / info / search / update ----
        got, seg = run(vm, mon, "/bin/vpkg list", "[VPKG] list ok packages=7")
        check("① `vpkg list` 列出仓库 7 个包（含安装状态）", got)
        check("① list 里 hello-cli 未装、索引字段齐全",
              'pkg name=hello-cli version=1.0 arch=vimtu64 type=vap64 size=' in seg
              and "state=not-installed" in seg)
        check("① list 里 gui-demo 的依赖条数 depends=1（索引里的 depends 关系读到了）",
              "pkg name=gui-demo" in seg and "depends=1" in seg)
        got, seg = run(vm, mon, "/bin/vpkg info hello-cli", "[VPKG] info ok name=hello-cli")
        check("① `vpkg info hello-cli`（+ installed=no）", got and "info installed=no" in seg)
        got, seg = run(vm, mon, "/bin/vpkg search demo", "[VPKG] search kw=demo hits=")
        m = re.search(r"\[VPKG\] search kw=demo hits=(\d+)", seg)
        check("① `vpkg search demo` 命中 >= 2（名字/摘要子串匹配）",
              got and m and int(m.group(1)) >= 2, m.group(0) if m else "")
        got, seg = run(vm, mon, "/bin/vpkg update", "[VPKG] update ok checked=")
        check("① `vpkg update` 打印 up-to-date（没有可升级项）", got and "up-to-date" in seg)

        # ---- ② 反例 1：坏 sha256（盘上不能留东西）----
        got, seg = run(vm, mon, "/bin/vpkg install brokenpkg", "[VPKG] done cmd=install rc=2",
                       expect_absent="[VPKG] install ok")
        m = re.search(r"\[VPKG\] sha256 path=(\S+) calc=(\w+) index=(\w+) ok=0", seg)
        check("② 反例：坏 sha256 被拒（rc=2，sha256 ok=0 原文在）", got and m is not None,
              m.group(0) if m else "")
        check("② 坏哈希不许落盘（没有 install ok / 没有 write）",
              "[VPKG] install ok" not in seg and "[VPKG] install file path=/bin/brokenpkg" not in seg)

        # ---- ③ 反例 2：缺依赖（gui-demo 依赖 hello-cli）----
        got, seg = run(vm, mon, "/bin/vpkg install gui-demo", "[VPKG] done cmd=install rc=4")
        check("③ 反例：缺依赖被拒（rc=4 + depends ... state=missing）",
              got and "depends name=gui-demo on=hello-cli state=missing" in seg
              and "ERROR code=depends" in seg)

        # ---- ④ 反例 3：空间不足（2 MiB payload vs 1.6 MB 卷）----
        got, seg = run(vm, mon, "/bin/vpkg install spacehog", "[VPKG] done cmd=install rc=5", timeout=90)
        m = re.search(r"\[VPKG\] space need=(\d+) free=(\d+)", seg)
        check("④ 反例：空间不足被拒（rc=5，need > free 原文在）",
              got and m is not None and int(m.group(1)) > int(m.group(2)),
              m.group(0) if m else "")

        # ---- ⑤ 装 hello-cli（.vap64）-> 真跑 -> 重复装被拒 ----
        got, seg = run(vm, mon, "/bin/vpkg install hello-cli", "[VPKG] install ok name=hello-cli")
        check("⑤ `.vap64` 装成功（install ok + 两个落盘文件）",
              got and "[VPKG] install file path=/bin/hello-cli " in seg
              and "[VPKG] install file path=/usr/share/hello-cli/hello-cli.vap64 " in seg)
        check("⑤ 装完写已装库（db write ... entries=1）", "[VPKG] db write path=/var/lib/vpkg/installed.json entries=1" in seg)
        hello_ok, seg = False, ""
        for _try in range(2):                       # 打字抖动 -> 重打一次；判据不放宽
            got, seg = run(vm, mon, "/bin/hello-cli", HELLO_CLI_MARK, timeout=40)
            if (HELLO_CLI_MARK in seg) and ("[HELLO-CLI] summary" in seg) and (HELLO_CLI_SUMMARY in seg):
                hello_ok = True
                break
        check("⑤ **装出来的程序真能跑**（run /bin/hello-cli 打出 SDK 模板的 hello + summary rc=0）",
              hello_ok, (re.findall(r"\[HELLO-CLI\][^\n]*", seg) or [""])[-1])
        got, seg = run(vm, mon, "/bin/vpkg install hello-cli", "[VPKG] done cmd=install rc=6")
        check("⑤ 反例：重复安装被拒（rc=6 + already installed）",
              got and "already installed name=hello-cli" in seg and "ERROR code=installed" in seg)

        # ---- ⑥ 装 gui-demo（依赖已满足）-> 卸载 -> 卸载后文件消失 ----
        got, seg = run(vm, mon, "/bin/vpkg install gui-demo", "[VPKG] install ok name=gui-demo")
        check("⑥ 依赖满足后 gui-demo 装成功（depends ... state=ok）",
              got and "depends name=gui-demo on=hello-cli state=ok" in seg)
        got, seg = run(vm, mon, "/bin/vpkg remove gui-demo", "[VPKG] remove ok name=gui-demo")
        check("⑥ `vpkg remove gui-demo`（remove file ... rc=0 + 已装库回到 1 条）",
              got and "[VPKG] remove file path=/bin/gui-demo rc=0" in seg and "entries=1" in seg)
        got, seg = run(vm, mon, "/bin/vpkg remove gui-demo", "[VPKG] done cmd=remove rc=7")
        check("⑥ 反例：卸载没装过的包被拒（rc=7 not-installed）", got and "ERROR code=not-installed" in seg)
        got, seg = run(vm, mon, "/bin/vpkg info gui-demo", "[VPKG] info ok name=gui-demo")
        check("⑥ info 里 gui-demo 回到 installed=no", got and "info installed=no" in seg)

        # ---- ⑦ deb 路：纯 payload 装成功 ----
        got, seg = run(vm, mon, "/bin/vpkg install vpkg-debdemo",
                       "[VPKG] install ok name=vpkg-debdemo")
        check("⑦ 真 .deb（ar + control.tar.gz + data.tar.gz）装成功",
              got and "deb control package=vpkg-debdemo version=1.0 arch=vimtu64" in seg)
        check("⑦ data.tar 的文件都落盘（包里两条路径都有 install file 打点）",
              "[VPKG] install file path=/usr/share/vpkg-debdemo/data.txt bytes=" in seg
              and "[VPKG] install file path=/usr/share/vpkg-debdemo/hello.txt bytes=" in seg)

        # ---- ⑧ 反例：带维护者脚本的 deb 整包拒绝 ----
        got, seg = run(vm, mon, "/bin/vpkg install baddeb", "[VPKG] done cmd=install rc=9")
        check("⑧ 反例：带 preinst 的 deb **整包拒绝**（rc=9 + 明确文案）",
              got and MAINTAINER_MSG in seg and "deb maintainer scripts: preinst=1" in seg)
        check("⑧ 被拒的 deb 零残留（没有 install file / 没有 install ok）",
              "[VPKG] install file path=/usr/share/baddeb" not in seg and "[VPKG] install ok" not in seg)

        # ---- ⑨ 反例：xz 压缩的 deb 明确拒绝 ----
        got, seg = run(vm, mon, "/bin/vpkg install xzdeb", "[VPKG] done cmd=install rc=10")
        check("⑨ 反例：xz 压缩的 deb 明确拒绝（rc=10 + 压缩格式原文）",
              got and "deb compression control=" in seg and "ERROR code=xz" in seg)

        logA = vm.log()
        for needle in PANIC_MARKERS:
            check("第一遍不得出现 %s" % needle, needle not in logA)
        vm.stop()

        # ---- ⑩ 关 QEMU 后：宿主侧解析系统卷 ----
        print("--- ⑩ 关 QEMU 后宿主侧解析系统卷（字节证据）---")
        vol = vol_bytes(FIXTURE_IMG)
        check("卷头是 VimtuFS2", vol[0:8] == b"VIMTUFS2")

        want_hello_payload = vap_payload(repo_host[by_name["hello-cli"]["file"]])
        got_bin = vol_read(tp, vol, "/bin/hello-cli")
        check("⑩ /bin/hello-cli 存在且 == .vap64 的 payload 段（逐字节 %d B）"
              % len(want_hello_payload), got_bin == want_hello_payload)
        got_share = vol_read(tp, vol, "/usr/share/hello-cli/hello-cli.vap64")
        check("⑩ /usr/share/hello-cli/hello-cli.vap64 == 仓库里的包本体（逐字节 %d B）"
              % len(repo_host[by_name["hello-cli"]["file"]]),
              got_share == repo_host[by_name["hello-cli"]["file"]])
        check("⑩ /bin/hello-cli 是静态 ELF64（装出来的东西可执行）",
              got_bin is not None and got_bin[:4] == b"\x7fELF" and got_bin[4] == 2)

        deb_files = deb_data_files(repo_host[by_name["vpkg-debdemo"]["file"]])
        for vpath, want in sorted(deb_files.items()):
            have = vol_read(tp, vol, "/" + vpath)
            check("⑩ deb 的 data.tar 内容逐字节在卷上：/%s（%d B）" % (vpath, len(want)),
                  have == want)
        check("⑩ /usr/share/vpkg-debdemo/vpkg-debdemo.deb == 仓库里的包本体",
              vol_read(tp, vol, "/usr/share/vpkg-debdemo/vpkg-debdemo.deb")
              == repo_host[by_name["vpkg-debdemo"]["file"]])

        db = vol_read(tp, vol, "/var/lib/vpkg/installed.json")
        check("⑩ /var/lib/vpkg/installed.json 存在（宿主独立解析）", db is not None)
        if db:
            txt = db.decode("utf-8", "replace")
            try:
                j = json.loads(txt)
            except Exception as ex:                            # noqa: BLE001
                j = None
                check("⑩ installed.json 是合法 JSON", False, str(ex))
            if j is not None:
                names = sorted(p["name"] for p in j["packages"])
                check("⑩ installed.json 记录 = {hello-cli, vpkg-debdemo}（卸载过的不在里面）",
                      names == ["hello-cli", "vpkg-debdemo"], str(names))
                hello = [p for p in j["packages"] if p["name"] == "hello-cli"][0]
                check("⑩ hello-cli 的 files[] 两条路径齐全 + sha256 与索引一致",
                      hello["files"] == ["/bin/hello-cli",
                                         "/usr/share/hello-cli/hello-cli.vap64"]
                      and hello["sha256"] == by_name["hello-cli"]["sha256"],
                      str(hello["files"]))

        # ---- ⑪ 卸载/被拒的东西在盘上不留痕迹 ----
        for vpath, why in (("/bin/gui-demo", "卸载后文件消失"),
                           ("/usr/share/gui-demo/gui-demo.vap64", "卸载后包本体副本消失"),
                           ("/bin/brokenpkg", "坏哈希零残留"),
                           ("/usr/share/baddeb", "带脚本的 deb 零残留"),
                           ("/usr/share/xzdeb", "xz 的 deb 零残留"),
                           ("/usr/share/spacehog/big.bin", "空间不足零残留")):
            check("⑪ %s：卷上没有 %s" % (why, vpath), vol_read(tp, vol, vpath) is None)

        # ================= 第二遍：GUI 路（同一块盘，状态延续） =================
        if args.no_gui:
            print("--- 跳过 GUI 路（--no-gui）---")
        else:
            print("=== 第二遍：GUI 商店（/bin/store：/lib/wm.elf 合成器 + font64 + 点击安装/启动）===")
            vm = Vm(qemu, FIXTURE_IMG, args.port + 1, os.path.join(tmp, "boot2.log"),
                    "vimtu-vpkg-gui", args.timeout)
            mon = vm.monitor(args.port + 1)
            qh.login_desktop(mon, vm.log, vm.proc, timeout=min(args.timeout, 180))
            check("桌面就绪 [GUI64] ready", vm.wait_mark("[GUI64] ready", args.timeout))
            opened = False
            for _ in range(3):
                mon.key("meta_l", wait=1.0)
                mon.key("1", wait=2.0)
                if vm.wait_mark("[APP] term opened", 12):
                    opened = True
                    break
            check("终端打开", opened)
            n0 = vm.n()
            type_line(mon, "shell", per_key=0.12)
            vm.wait_mark("[SH64] launch", 40)
            type_line(mon, "run /bin/store", per_key=0.09)
            # 合成器 + 窗口
            check("[STORE] 起来了（ver=1 pid=..）",
                  vm.wait_re(r"\[STORE\] ver=1 pid=(\d+)", 60) is not None)
            m = re.search(r"\[STORE\] ver=1 pid=(\d+)", vm.log()[n0:])
            store_pid = int(m.group(1)) if m else -1
            check("[STORE] 拉起 /lib/wm.elf（wm path=/lib/wm.elf pid=..）",
                  vm.wait_mark("[STORE] wm path=/lib/wm.elf pid=", 60))
            check("合成器注册成功（[WL64] composer pid=..）",
                  vm.wait_re(r"\[WL64\] composer pid=(\d+)", 60) is not None)
            check("合成器在动（[WM] frame .. surfs=..）",
                  vm.wait_re(r"\[WM\] frame n=\d+ surfs=\d+", 60) is not None)
            check("[STORE] 字体从系统卷读（fonts face=/Fonts/NotoSans-Regular.ttf ok=1）",
                  vm.wait_mark("[STORE] fonts face=/Fonts/NotoSans-Regular.ttf bytes=", 60)
                  and "ok=1" in vm.log()[n0:])
            ms = vm.wait_re(r"\[STORE\] surf id=(\d+) w=256 h=60 shm=(\d+) map=0", 60)
            check("[STORE] 窗口 = 256x60 shm 画布（单 surface <= 16384 px）", ms is not None,
                  ms.group(0) if ms else "")
            check("[STORE] 仓库读到 7 个包 / 已装 2 个（第一遍的状态延续）",
                  vm.wait_re(r"\[STORE\] repo packages=7 installed=2", 60) is not None)
            mr = vm.wait_re(r"\[STORE\] render f=0 sel=0 row0=hello-cli row1=gui-demo "
                            r"row2=vpkg-debdemo state0=installed", 60)
            check("[STORE] 列表渲染打点（row0=hello-cli(installed) row1=gui-demo row2=vpkg-debdemo）",
                  mr is not None, mr.group(0) if mr else "")
            # 窗口在屏上的位置（内核打点）+ 指针闭环用的 surface 原点
            mw = vm.wait_re(r"\[WL64\] surface create id=(\d+) w=256 h=60 shm=\d+ pid=%d px=\d+ "
                            r"fmt=\d+ slot=\d+ pos=(-?\d+),(-?\d+)" % (store_pid, ), 60)
            check("内核 surface 表里有商店的窗口（带屏上坐标 pos=x,y）", mw is not None,
                  mw.group(0) if mw else "")
            panel_x = int(mw.group(2)) if mw else 964
            panel_y = int(mw.group(3)) if mw else 532
            print("[vpkg64] 商店窗口屏上原点 = (%d, %d)" % (panel_x, panel_y))
            time.sleep(1.0)

            # ---- 像素/墨迹断言：列表渲染 ----
            ppm = os.path.join(tmp, "store_list.ppm")
            got = mon.shot(ppm)
            check("截到商店窗口的画面（screendump PPM）", got)
            shot = read_ppm(ppm) if got else None
            if shot:
                w, h, px = shot
                rect = (panel_x, panel_y, panel_x + STORE_W, panel_y + STORE_H)
                bright = count_bright(px, w, h, rect, 120)
                check("列表渲染有墨迹（窗口里亮像素 >= 300：标题 + 三行文字）", bright >= 300,
                      "bright=%d" % bright)
                green = count_color(px, w, h, CO_OK, 20, rect)
                check("第 1 行（hello-cli 已装）的**绿图标**在屏上（>= 40 px）", green >= 40,
                      "green=%d" % green)
                sel = count_color(px, w, h, CO_ROWSEL, 12, rect)
                check("选中行底色在屏上（>= 200 px）", sel >= 200, "sel=%d" % sel)

            pad = Pad(mon, vm, start=(512, 384))

            def click_panel(local_x, local_y, want, tries=3, wait=1.4):
                """点窗口内的 (local_x, local_y)：先用模型走过去，再用 store 报的坐标闭环校正。"""
                tx, ty = panel_x + local_x, panel_y + local_y
                n1 = vm.n()
                for k in range(tries):
                    pad.move_to(tx, ty, tol=3)
                    # 用 store 自报的鼠标坐标校正（它只在指针**落在窗口里**时才报）
                    last = last_store_mouse(vm, None)
                    if last:
                        pad.resync(last[0], last[1])
                    mon.move(0, 0, wait=0.05)
                    mon.click(wait=0.8)
                    if vm.wait_mark(want, wait, since=n1):
                        return True
                    tx += (4 if k % 2 == 0 else -8)
                    ty += (3 if k % 3 == 0 else -5)
                return False

            # ---- 点选第 2 行（gui-demo）----
            sel_ok = click_panel(64, ROW0_Y + ROW_H + ROW_H // 2,
                                 "hit=row sel=1 name=gui-demo", tries=3)
            check("点第 2 行 -> 选中 gui-demo（[STORE] click ... hit=row sel=1）", sel_ok,
                  (re.findall(r"\[STORE\] click[^\n]*", vm.log()) or [""])[-1])

            # ---- 点「安装」----
            n1 = vm.n()
            ins_ok = click_panel(BTN_INSTALL_X + BTN_INSTALL_W // 2, BTN_Y + BTN_H // 2,
                                 "hit=install", tries=3)
            check("点「安装」按钮（命中测试 hit=install）", ins_ok)
            check("点击 -> 安装开始（[STORE] click install name=gui-demo）",
                  vm.wait_mark("[STORE] click install name=gui-demo", 5, since=n1))
            check("安装进度有界打点（progress pct=25/40/70/100）",
                  vm.wait_mark("[STORE] progress pct=100", 30, since=n1))
            check("**GUI 点安装 -> 包真进卷**（[STORE] install ok name=gui-demo）",
                  vm.wait_mark("[STORE] install ok name=gui-demo", 30, since=n1))
            seg = vm.log()[n1:]
            check("GUI 的安装走的是同一份引擎（install file + db write 都在）",
                  "[STORE] install file path=/bin/gui-demo " in seg
                  and "[STORE] db write path=/var/lib/vpkg/installed.json entries=3" in seg)

            time.sleep(0.8)
            ppm2 = os.path.join(tmp, "store_installed.ppm")
            got2 = mon.shot(ppm2)
            check("装完再截一帧（像素证据）", got2)
            shot2 = read_ppm(ppm2) if got2 else None
            if shot2:
                w, h, px = shot2
                rect2 = (panel_x + 2, panel_y + ROW0_Y + ROW_H + 2,
                         panel_x + 10, panel_y + ROW0_Y + ROW_H + 10)
                green2 = count_color(px, w, h, CO_OK, 20, rect2)
                check("第 2 行的图标变成**绿色**（装完了；绿像素 >= 40）", green2 >= 40,
                      "green2=%d" % green2)
                full = (panel_x, panel_y, panel_x + STORE_W, panel_y + STORE_H)
                check("状态条/图标带整体绿像素增多（>= 120）",
                      count_color(px, w, h, CO_OK, 20, full) >= 120)

            # ---- 点「启动」----
            n2 = vm.n()
            la_ok = click_panel(BTN_LAUNCH_X + BTN_LAUNCH_W // 2, BTN_Y + BTN_H // 2,
                                "hit=launch", tries=3)
            check("点「启动」按钮（命中测试 hit=launch）", la_ok)
            ml = vm.wait_re(r"\[STORE\] launch name=gui-demo path=/bin/gui-demo pid=(\d+)", 20, since=n2)
            check("**GUI 点启动 -> 装出来的包真跑起来**（[STORE] launch ... pid=N）", ml is not None,
                  ml.group(0) if ml else "")
            check("被启动的程序自己的打点（[DEMO-GUI] ver=1 .. done frames=3）",
                  vm.wait_mark("[DEMO-GUI] done frames=3", 30, since=n2),
                  (re.findall(r"\[DEMO-GUI\][^\n]*", vm.log()[n2:]) or [""])[-1])
            check("被启动的程序提交了 3 帧（[DEMO-GUI] frame f=0/1/2 rc=0/0）",
                  len(re.findall(r"\[DEMO-GUI\] frame f=\d+ col=0x\w+ rc=0/0",
                                 vm.log()[n2:])) >= 3)
            check("它的窗口也进了内核 surface 表（[WL64] surface create w=160 h=40）",
                  vm.wait_re(r"\[WL64\] surface create id=\d+ w=160 h=40", 20, since=n2) is not None)

            # ---- ESC 退出商店（有界收尾）----
            mon.key("esc", wait=1.5)
            check("ESC 退出商店（[STORE] done frames=.. reason=esc）",
                  vm.wait_mark("[STORE] done frames=", 30))
            logB = vm.log()
            for needle in PANIC_MARKERS:
                check("第二遍不得出现 %s" % needle, needle not in logB)
            vm.stop()

            # ---- GUI 之后的宿主侧复核（同一块盘，状态延续）----
            print("--- GUI 之后宿主侧复核（/bin/gui-demo 是否真进卷）---")
            vol2 = vol_bytes(FIXTURE_IMG)
            want_gui = vap_payload(repo_host[by_name["gui-demo"]["file"]])
            got_gui = vol_read(tp, vol2, "/bin/gui-demo")
            check("GUI 安装的 gui-demo 真进卷（/bin/gui-demo == .vap64 payload，%d B）"
                  % len(want_gui), got_gui == want_gui)
            check("/usr/share/gui-demo/gui-demo.vap64 也在（包本体副本）",
                  vol_read(tp, vol2, "/usr/share/gui-demo/gui-demo.vap64")
                  == repo_host[by_name["gui-demo"]["file"]])
            db2 = vol_read(tp, vol2, "/var/lib/vpkg/installed.json")
            if db2:
                j2 = json.loads(db2.decode("utf-8", "replace"))
                names = sorted(p["name"] for p in j2["packages"])
                check("GUI 装完后 installed.json = {gui-demo, hello-cli, vpkg-debdemo}",
                      names == ["gui-demo", "hello-cli", "vpkg-debdemo"], str(names))
                gui = [p for p in j2["packages"] if p["name"] == "gui-demo"][0]
                check("gui-demo 记录的 files[] 与 sha256 正确",
                      gui["files"] == ["/bin/gui-demo", "/usr/share/gui-demo/gui-demo.vap64"]
                      and gui["sha256"] == by_name["gui-demo"]["sha256"], str(gui["files"]))
            else:
                check("GUI 之后 installed.json 读得到", False)
    finally:
        try:
            if monkey:
                pass
        except Exception:                                      # noqa: BLE001
            pass
        try:
            if 'vm' in dir():
                vm.stop()
        except Exception:                                      # noqa: BLE001
            pass

    # ---- 串口原文（证据要贴的那几行）----
    for tag, log in (("boot1/CLI", logA), ("boot2/GUI", logB)):
        if not log:
            continue
        print("--- %s 的 [VPKG]/[STORE]/[DEMO-*] 串口原文（截断到 40 行）---" % tag)
        for line in [x.strip() for x in log.splitlines()
                     if any(k in x for k in ("[VPKG]", "[STORE]", "[DEMO-CLI]", "[DEMO-GUI]"))][:40]:
            print("   | " + line[:200])

    if args.keep:
        print("[vpkg64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _n, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
