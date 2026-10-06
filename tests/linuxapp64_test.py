#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/linuxapp64_test.py - ★「Linux 应用能不能装、能不能跑」验收骨架（**缺口清单的载体**）

一句话：本测试**不修内核、不放宽断言** —— 它按"用户想要的最终行为"写断言（发行版程序能装、
能跑、能打印它自己的输出），今天在 ②③④ 上**如实 FAIL**，并把**内核串口原文**（[ELF64] …、
[SYSCALL] …、[PROC64] …）连同缺口一起打出来；①（静态 musl，对照组）今天必须 PASS。

语料（宿主侧造，见 tools/lxcorpus_build.py；本脚本只读 build64/lxcorpus/CORPUS.json）：

  ①  /lxcorpus/stathello.elf            静态 musl、钉在 4GiB、无 PT_INTERP   期望：**今天就能跑**
  ①c /lxcorpus/bighello.elf             静态 musl，但 425 KB > 内核 96 KiB 读盘上限
  ①  /lxcorpus/musl_hello.elf           仓库既有的 musl 静态程序（证明夹具/打字/日志这条链没问题）
  ②  /lxcorpus/dynhello.elf             动态 musl：PT_INTERP=/lib/ld-musl-x86_64.so.1、
                                        DT_NEEDED=libc.so（卷里也装了这两个文件）
  ②b /lxcorpus/dynglibc.elf             同一个程序，PT_INTERP 换成 /lib64/ld-linux-x86-64.so.2
                                        （卷里也装了真的 glibc 2.36 解释器）——把内核带到"解释器那一步"
  ③  /lxcorpus/gnuhello                 Debian bookworm 的 hello 包里的真 /usr/bin/hello（PIE + glibc）
  ④  /opt/vpkg/repo/*.deb               Debian 原样的 .deb（xz）/ gzip 重压（带 Depends）/ gzip（去 Depends）
                                        / gzip（去 Depends + 只留 1 个文件）
  ⑤  /lxcorpus/dynhello-with-a-long-name.elf
                                        同一个二进制换成长路径：内核 execve(59) 的路径缓冲只有 32 B
                                        （kernel/syscall64.cpp:579 LX64_PATH_MAX）→ deny + EFAULT

为什么 ② 用"解释器-load"类断言、③ 用"主程序-load"类断言：
  kernel/elf64.cpp 的顺序是 **先解析主程序**（elf64_parse64 → e64_lo64/e64_hi64 = 4GiB..4GiB+64KiB、
  base=0、没有 load bias），**再**去装 PT_INTERP（e64_load_interp64）。所以
  * 发行版二进制（PIE，p_vaddr=0；老式非 PIE，0x400000）一律在**主程序**这一步就被拒
    （[ELF64] exec reject segment va=… reason=outside-user-window）；
  * 只有"自己链在 4GiB 的程序 + 真解释器"才能走到解释器那一步，撞上内核对解释器的两道硬上限：
    读盘缓冲 96 KiB（kernel/elf64.h:80）与解释器窗口 448 KiB（usermode64.h:51 + elf64.cpp:584）。
  两类失败点因此是**分开测**的，不是同一条。

用法（**必须 Windows 原生 Python**；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tools/lxcorpus_build.py         # 先造语料（一次即可；产物在 build64/lxcorpus/）
    py -3 tests\\linuxapp64_test.py [--qemu PATH] [--keep] [--no-qemu]
退出码：0 = 全过（**这一批之后应当 36/36**）；1 = 有断言失败（= 缺口清单）；2 = 环境问题。
★ 这一批（deb 链路补齐）把 ④ 的三条"缺口跟踪"断言**收紧**成成功断言，并新增两条证据：
  * 装完**完整包**（原样 xz、49 条目 data.tar）之后当场 `run /usr/bin/hello` 打印 Hello, world!；
  * 关 QEMU 后宿主侧用**独立的** lzma/tarfile 解析那个原样 .deb，逐字节核对每个 payload 条目。
  （断言只增不减/只紧不松：条数仍是 36，没有一条放松或删除。）

★ 本脚本不含任何"为了变绿"的写法：每条 FAIL 都打印"想要什么 / 实际拿到什么 / 内核原话"。
"""
import argparse
import hashlib
import importlib.util
import json
import gzip
import io
import lzma
import tarfile
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

CORPUS = os.path.join(ROOT, "build64", "lxcorpus")
CORPUS_JSON = os.path.join(CORPUS, "CORPUS.json")
SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
VPKG_ELF = os.path.join(ROOT, "build64", "store", "vpkg.elf")
FIXTURE_REPO = os.path.join(CORPUS, "repo_fixture")
FIXTURE_IMG = os.path.join(ROOT, "build64", "lxcorpus_test.img")
FRESH_VOL_SECTORS = 16384               # 8 MiB 卷（语料 ~3.7 MB 的库 + 装出来的东西都放得下）
PART_MAIN_LBA = 8009

# 内核打点里"这一类的失败原因"的原文（用于把缺口钉死；都是今天**应当出现**的）
REASON_SIZE = "[ELF64] exec reject reason=size"
REASON_INTERP_SIZE = "[ELF64] exec reject reason=interp-size"
REASON_WINDOW = "reason=outside-user-window"
PANIC_MARKERS = ["PANIC", "TRIPLE FAULT", "FAILED mask="]

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}


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


def sha256_of(p):
    with open(p, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def load_spw():
    path = os.path.join(ROOT, "tools", "store_pack_win.py")
    spec = importlib.util.spec_from_file_location("spw", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------
# QEMU + monitor（与 tests/vpkg64_test.py 同一套）
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
            except Exception:                                  # noqa: BLE001
                pass


def type_line(mon, text, per_key=0.09):
    for ch in text:
        if ch in KEYMAP:
            mon.key(KEYMAP[ch], wait=per_key)
        elif ch.isalnum():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.3)


# ---------------------------------------------------------------------------
# 夹具：语料体检 + 造盘
# ---------------------------------------------------------------------------
def corpus_items():
    if not os.path.exists(CORPUS_JSON):
        raise RuntimeError("缺 %s —— 先跑 `py -3 tools/lxcorpus_build.py`" % CORPUS_JSON)
    with open(CORPUS_JSON, encoding="utf-8") as f:
        man = json.load(f)
    return man


def fixture_repo_index(items):
    """按 vpkg 的 index.json 形状写一份夹具仓库（两个真 .deb：原样 xz / gz 重压）。"""
    pkgs = []
    os.makedirs(FIXTURE_REPO, exist_ok=True)
    for e in items:
        if e["kind"] != "deb":
            continue
        src = e["host"]
        p = os.path.join(FIXTURE_REPO, os.path.basename(src))
        shutil.copyfile(src, p)
        pkgs.append({"name": e["pkg"], "version": "2.10-2",
                     "arch": "amd64", "type": "deb", "file": os.path.basename(src),
                     "size": os.path.getsize(p), "sha256": sha256_of(p), "depends": [],
                     "summary": "Debian hello (%s)" % ("xz" if e["id"].endswith("xz") else "gzip repack"),
                     "description": e["note"]})
    index = {"schema": 1, "generator": "tests/linuxapp64_test.py",
             "repo": "/opt/vpkg/repo", "packages": pkgs}
    os.makedirs(FIXTURE_REPO, exist_ok=True)
    with open(os.path.join(FIXTURE_REPO, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=1)
        f.write("\n")
    return index


def prepare_fixture(spw, man, verbose=True):
    """用 tools/store_pack_win.py 的**公开路径**造夹具盘：语料逐文件装进 VimtuFS2 卷。"""
    for p in (SYSTEM_IMG, SHELL_BIN, VPKG_ELF):
        if not os.path.exists(p):
            raise RuntimeError("缺 %s（先跑 bash build64.sh）" % p)
    index = fixture_repo_index(man["items"])
    srcs = [(SHELL_BIN, "/bin/shell.bin", 0o755), (VPKG_ELF, "/bin/vpkg", 0o755)]
    for e in man["items"]:
        srcs.append((e["host"], e["vol"], int(e["mode"], 8)))
    cmd = [sys.executable, os.path.join(ROOT, "tools", "store_pack_win.py"),
           "--fixture-img", FIXTURE_IMG, "--fresh-vol", str(FRESH_VOL_SECTORS),
           "--repo", FIXTURE_REPO, "--system", SYSTEM_IMG, "--writable-roots"]
    for host, vol, mode in srcs:
        cmd += ["--src", "%s:%s:%o" % (host, vol, mode)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if verbose:
        print(r.stdout[-3000:])
        if r.stderr:
            print(r.stderr[-2000:])
    if r.returncode != 0:
        raise RuntimeError("造夹具盘失败（rc=%d）" % r.returncode)
    return index


def vol_bytes(img):
    with open(img, "rb") as f:
        f.seek(PART_MAIN_LBA * 512)
        return f.read(FRESH_VOL_SECTORS * 512)


def vol_read(tp, vol, path):
    """宿主侧按 VimtuFS2 格式独立读一个路径（多级目录；不依赖内核/vpkg 的自述）。"""
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


def deb_payload_files(path):
    """宿主侧**独立**解析一个 .deb 的 data.tar.{xz,gz}（用 Python 自己的 lzma/gzip/tarfile，
    与客人里 /bin/vpkg 的实现毫无共享代码）：返回 {卷内相对路径: 字节}（只取常规文件）。
    用途：关 QEMU 之后逐字节核对"包管理器到底把包的 payload 原样写进卷没有"。"""
    blob = open(path, "rb").read()
    if blob[:8] != b"!<arch>\n":
        raise RuntimeError("%s 不是 ar 归档" % path)
    members, off = {}, 8
    while off + 60 <= len(blob):
        h = blob[off:off + 60]
        nm = h[0:16].decode("ascii").split("/")[0].strip()
        sz = int(h[48:58].decode("ascii").strip() or "0")
        members[nm] = blob[off + 60:off + 60 + sz]
        off = off + 60 + sz + (sz & 1)
    for key in ("data.tar.xz", "data.tar.gz"):
        if key not in members:
            continue
        raw = lzma.decompress(members[key]) if key.endswith("xz") else gzip.decompress(members[key])
        out = {}
        with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
            for ti in tf.getmembers():
                if not ti.isfile():
                    continue
                nm = ti.name[2:] if ti.name.startswith("./") else ti.name
                out[nm] = tf.extractfile(ti).read()
        return out
    raise RuntimeError("%s 里没有 data.tar.{xz,gz}" % path)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5711)
    ap.add_argument("--timeout", type=int, default=220)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-qemu", action="store_true", help="只做宿主侧体检（语料 + 夹具盘）")
    args = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                      # noqa: BLE001
            pass

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def raw(seg, keys=("[ELF64]", "[SYSCALL]", "[PROC64]", "[SH64]", "run: ", "Hello")):
        out = [x.strip() for x in seg.splitlines() if any(k in x for k in keys)]
        key = [x for x in out if ("reject" in x or "deny" in x or "enosys" in x or "FAILED" in x)]
        tail = [x for x in out[-4:] if x not in key]
        return " | ".join((key + tail)[:6])[:700]

    print("=== Vimtu64 「Linux 应用：能装 / 能跑」验收（语料逐类跑，缺口如实列出）===")
    try:
        man = corpus_items()
    except Exception as ex:                                    # noqa: BLE001
        sys.stderr.write("语料体检失败（环境问题）：%s\n" % ex)
        return 2
    items = {e["id"]: e for e in man["items"]}
    lim = man["kernel_limits"]

    # ---------- 宿主侧：语料 + 夹具盘 ----------
    print("--- 宿主侧：语料体检（sha256）+ 造夹具盘 ---")
    bad = []
    for e in man["items"]:
        if not os.path.exists(e["host"]):
            bad.append("%s 缺文件" % e["id"])
        elif sha256_of(e["host"]) != e["sha256"]:
            bad.append("%s sha256 对不上（重建语料：py -3 tools/lxcorpus_build.py）" % e["id"])
    check("①~④ 语料 %d 项齐全且 sha256 与清单一致" % len(man["items"]), not bad, "；".join(bad))
    if bad:
        return 2

    try:
        spw = load_spw()
        index = prepare_fixture(spw, man)
    except Exception as ex:                                    # noqa: BLE001
        sys.stderr.write("夹具失败（环境问题）：%s\n" % ex)
        return 2
    check("④ 夹具仓库 index.json 有 %d 个真 .deb（xz 原样 / gz / gz-去依赖 / gz-最小）"
          % len(index["packages"]), len(index["packages"]) == len([e for e in man["items"]
                                                                  if e["kind"] == "deb"]))
    if args.no_qemu:
        print("=== RESULT: %s ===  checks=%d ok=%d（--no-qemu：只体检宿主侧）"
              % ("PASS" if ok else "FAIL", len(checks), sum(1 for _n, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_lxapp_")
    log = ""
    segs = {}
    try:
        vm = Vm(qemu, FIXTURE_IMG, args.port, os.path.join(tmp, "boot.log"), "vimtu-lxapp", args.timeout)
        mon = vm.monitor(args.port)
        qh.login_desktop(mon, vm.log, vm.proc, timeout=min(args.timeout, 180))
        check("桌面就绪 [GUI64] ready", vm.wait_mark("[GUI64] ready", args.timeout))
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if vm.wait_mark("[APP] term opened", 12):
                opened = True
                break
        check("终端打开（[APP] term opened）", opened)
        type_line(mon, "shell", per_key=0.12)
        check("ring3 外壳 /bin/shell.bin 起来了（[SH64] launch）",
              vm.wait_re(r"\[SH64\] launch path=/bin/shell\.bin size=(\d+) pid=(\d+)", 40) is not None)
        vm.wait_mark("VimtuOS ring3 shell", 20)

        def run_prog(path, want=None, timeout=70, expect_code=None):
            """在 ring3 外壳里 `run <path>`：等到"想要的输出"或外壳的汇总行（run: … exited code=）。
            两者都要给它时间落盘 —— 找到 want 之后还要再等一小会儿，才能拿到汇总行。"""
            n0 = vm.n()
            type_line(mon, "run " + path)
            rx = re.compile(r"run: %s pid=\d+ exited code=(\d+)" % re.escape(path))
            t0 = time.time()
            got_want = False
            while time.time() - t0 < timeout:
                seg = vm.log()[n0:]
                if want and want in seg:
                    got_want = True
                m = rx.search(seg)
                if m and (got_want or not want or expect_code is None):
                    time.sleep(0.6)                      # 汇总行之后通常还有收尸打点
                    return vm.log()[n0:]
                if got_want and expect_code is None:
                    time.sleep(0.6)
                    return vm.log()[n0:]
                time.sleep(0.2)
            return vm.log()[n0:]

        def run_shell_line(cmd, settle, timeout=120):
            n0 = vm.n()
            type_line(mon, cmd)
            t0 = time.time()
            while time.time() - t0 < timeout:
                seg = vm.log()[n0:]
                if settle in seg:
                    time.sleep(0.8)
                    return vm.log()[n0:]
                time.sleep(0.2)
            return vm.log()[n0:]

        # ================= ① 静态 musl（对照组：今天必须 PASS）=================
        print("--- ① 静态 musl（对照组）---")
        v = items["static_musl"]["vol"]
        seg = run_prog(v, want=man["marks"]["static"], expect_code=0)
        segs["stat"] = seg
        check("① `run %s` 打印语料自己的固定串（%d B）" % (v, items["static_musl"]["size"]),
              man["marks"]["static"] in seg, raw(seg))
        m = re.search(r"run: %s pid=\d+ exited code=(\d+)" % re.escape(v), seg)
        check("① 静态 musl 正常退出（exited code=0）", m is not None and m.group(1) == "0",
              m.group(0) if m else raw(seg))

        v = items["static_musl_repo"]["vol"]
        seg = run_prog(v, want=items["static_musl_repo"]["expect_out"], expect_code=0)
        segs["musl_repo"] = seg
        check("① 仓库既有 %s 照旧能跑（证明夹具/打字/日志这条链没问题）" % v,
              items["static_musl_repo"]["expect_out"] in seg, raw(seg))

        # ================= ①c 静态但 > 96 KiB（内核读盘上限）=================
        print("--- ①c 静态 musl，425 KB（> 内核 96 KiB 读盘上限）---")
        v = items["static_musl_big"]["vol"]
        seg = run_prog(v, want="[LXBIG]", timeout=45)
        segs["big"] = seg
        check("①c `run %s` 能跑起来（%d B；内核读盘上限 %d B）"
              % (v, items["static_musl_big"]["size"], lim["elf_max_file_bytes"]),
              "[LXBIG]" in seg, raw(seg))
        check("[缺口已修] ①c 不再撞读盘上限（无 reason=size；%d B 走**按段分块**读盘）"
              % items["static_musl_big"]["size"],
              REASON_SIZE not in seg and "[ELF64] load path=" in seg and "layout=1" in seg, raw(seg))

        # ================= ② 动态 musl =================
        print("--- ② 动态 musl（PT_INTERP=%s，卷里已装 %d KB 的 ld-musl）---"
              % (items["musl_dyn"]["note"].split("=")[-1], items["musl_ldso"]["size"] // 1024))
        v = items["musl_dyn"]["vol"]
        seg = run_prog(v, want=man["marks"]["dyn"])
        segs["dyn"] = seg
        check("② `run %s` 打印它自己的固定串（= 动态链接真的跑起来）" % v,
              man["marks"]["dyn"] in seg, raw(seg))
        check("[缺口已修] ② 的解释器真的装进来了（ld-musl %d B；[ELF64] interp path=… base=…，无 interp reject）"
              % items["musl_ldso"]["size"],
              REASON_INTERP_SIZE not in seg and "interp reject" not in seg and
              "[ELF64] interp path=/lib/ld-musl-x86_64.so.1 base=" in seg, raw(seg))

        print("--- ②b 同一个程序 + 真 glibc 解释器（%s，%d KB）---"
              % (items["glibc_ldso"]["vol"], items["glibc_ldso"]["size"] // 1024))
        v = items["gnu_interp_probe"]["vol"]
        seg = run_prog(v, want=man["marks"]["dyn"])
        segs["dyn_gnu"] = seg
        check("②b `run %s` 打印它自己的固定串" % v, man["marks"]["dyn"] in seg, raw(seg))
        check("[缺口已修] ②b 的真 glibc 解释器被内核装载（ld-linux %d B，无 interp reject）"
              % items["glibc_ldso"]["size"],
              REASON_INTERP_SIZE not in seg and "interp reject" not in seg and
              "[ELF64] interp path=/lib64/ld-linux-x86-64.so.2 base=" in seg, raw(seg))

        # ================= ⑤ 长路径（execve 路径缓冲 32 B）=================
        print("--- ⑤ 同一个二进制换个 38 字节的长路径（内核 execve 路径缓冲只有 32 B）---")
        v = items["dynhello_longpath"]["vol"]
        seg = run_prog(v, want=man["marks"]["dyn"], timeout=45)
        segs["longpath"] = seg
        check("⑤ `run %s`（%d 字符）能进 execve 并跑起来" % (v, len(v)),
              man["marks"]["dyn"] in seg, raw(seg))
        check("[缺口已修] ⑤ 的路径缓冲放宽到 128 B（无 deny nr=59；39 字符路径真的进了 execve）",
              "deny nr=59" not in seg and
              "[ELF64] load path=/lxcorpus/dynhello-with-a-long-name.elf" in seg, raw(seg))

        # ================= ③ 真发行版（Debian PIE + glibc）=================
        print("--- ③ 真发行版程序：Debian hello 的 /usr/bin/hello（PIE + glibc）---")
        v = items["glibc_bin"]["vol"]
        seg = run_prog(v, want="Hello, world!")
        segs["gnu"] = seg
        check("③ `run %s` 打印 \"Hello, world!\"（真发行版二进制能跑）" % v,
              "Hello, world!" in seg, raw(seg))
        check("[缺口已修] ③ 的 PIE 主程序按**程序头布局 + load bias**装载（无 outside-user-window）",
              REASON_WINDOW not in seg and "[ELF64] load path=/lxcorpus/gnuhello" in seg, raw(seg))

        # ================= ④ 真 .deb 通过 /bin/vpkg =================
        print("--- ④ 真 .deb：原样 xz / gzip（带 Depends）/ gzip（去 Depends）---")
        seg = run_shell_line("/bin/vpkg install hello-xz", "[VPKG] done cmd=install")
        segs["deb_xz"] = seg
        check("④ 发行版**原样** .deb（control/data.tar.xz）能被 /bin/vpkg 装上"
              "（xz 解码打点 + 49 个条目 + 全部落盘）",
              "[VPKG] install ok" in seg and "[VPKG] done cmd=install rc=0" in seg
              and "deb compression control=xz data=xz" in seg
              and "deb xz member=control.tar.xz in=" in seg
              and "deb xz member=data.tar.xz in=" in seg
              and "deb stats files=49 dirs=94" in seg
              and "[VPKG] install file path=/usr/bin/hello " in seg, raw(seg))
        check("[缺口已修] ④ 的两段 xz 真的解开了（成员 in/out 打点对上真包："
              "control 1868->10240、data 54072->256000；不再有 rc=10/only gzip）",
              "deb xz member=control.tar.xz in=1868 out=10240" in seg
              and "deb xz member=data.tar.xz in=54072 out=256000" in seg
              and "rc=10" not in seg and "only gzip is supported" not in seg, raw(seg))

        # ★ 新增证据（完整包就地跑）：装完**原样 xz 包**之后，它的 /usr/bin/hello 当场跑一遍 ——
        #   不是只看"install ok"，而是真 fork/exec 那个被包管理器写进卷的程序。
        seg_run_full = run_prog(items["deb_real_xz"]["payload"], want="Hello, world!", timeout=70)
        segs["deb_run_full"] = seg_run_full

        seg = run_shell_line("/bin/vpkg install hello-dep", "[VPKG] done cmd=install")
        segs["deb_dep"] = seg
        check("④ gzip 重压但**保留 control 里的 Depends** 的 .deb 能装上"
              "（Debian hello 声明 Depends: libc6 (>= 2.34)）",
              "[VPKG] install ok" in seg and "[VPKG] done cmd=install rc=0" in seg
              and "depends name=hello on=libc6 state=ok via=provides witness=/lib/x86_64-linux-gnu/libc.so.6" in seg,
              raw(seg))
        check("[缺口已修] ④ 的缺失依赖由 **provides 表（带见证路径）** 满足，不是被跳过"
              "（state=ok via=provides；没有 rc=4 / depends missing / state=forced）",
              "state=ok via=provides" in seg and "rc=4" not in seg
              and "deb depends missing=" not in seg and "state=forced" not in seg, raw(seg))

        seg = run_shell_line("/bin/vpkg install hello", "[VPKG] done cmd=install")
        segs["deb_gz"] = seg
        check("④ 去 Depends 但仍带**完整 data.tar（49 个条目）**的 .deb 能装上",
              "[VPKG] install ok" in seg and "[VPKG] done cmd=install rc=0" in seg, raw(seg))
        check("[缺口已修] ④ 的文件数上限（VS_FILES_MAX 12 -> 4096）：49 个条目全部落盘、"
              "预扫与落盘对账（deb stats files=49 / deb written files=49），不再 rc=8",
              "deb stats files=49 dirs=94" in seg and "cap=4096" in seg
              and "deb written files=49 bytes=" in seg and "rc=8" not in seg
              and "[VPKG] install ok" in seg, raw(seg))

        seg = run_shell_line("/bin/vpkg install hello-min", "[VPKG] done cmd=install")
        segs["deb_min"] = seg
        check("④ 同一份 payload 剥到最小（只留 /usr/bin/hello）后装成功"
              "（install ok + /usr/bin/hello 落盘）",
              "[VPKG] install ok" in seg and "[VPKG] install file path=/usr/bin/hello" in seg,
              raw(seg))

        v = items["deb_real_min"]["payload"]
        seg = run_prog(v, want="Hello, world!")
        segs["deb_run"] = seg
        check("④ **装出来的程序真能跑**（run %s 打印 \"Hello, world!\"；"
              "上面那份**完整包**装完也当场跑出同样一行）" % v,
              "Hello, world!" in seg and "Hello, world!" in seg_run_full, raw(seg))
        check("[缺口已修] ④ 装出来的程序走同一条装载路径（无 outside-user-window / reason=size）",
              REASON_WINDOW not in seg and REASON_SIZE not in seg and
              "[ELF64] load path=/usr/bin/hello" in seg, raw(seg))

        log = vm.log()
        for needle in PANIC_MARKERS:
            check("全程不得出现 %s" % needle, needle not in log)
        vm.stop()
    finally:
        try:
            if 'vm' in dir():
                vm.stop()
        except Exception:                                      # noqa: BLE001
            pass

    # ---------- 关 QEMU 后：宿主侧字节证据 ----------
    print("--- 关 QEMU 后：宿主侧解析系统卷（字节证据）---")
    try:
        spec = importlib.util.spec_from_file_location("tp", os.path.join(ROOT, "tools", "tcc_pack_win.py"))
        tp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tp)
        vol = vol_bytes(FIXTURE_IMG)
        check("卷头是 VimtuFS2", vol[0:8] == b"VIMTUFS2")
        for iid in ("static_musl", "musl_dyn", "glibc_bin", "static_musl_big"):
            e = items[iid]
            got = vol_read(tp, vol, e["vol"])
            check("⑩ 卷上 %s 与语料逐字节一致（%d B）" % (e["vol"], e["size"]),
                  got is not None and sha256_of(e["host"]) == hashlib.sha256(got).hexdigest(),
                  "卷上 %s" % (len(got) if got is not None else "没找到"))
        # ④ 装出来的 /usr/bin/hello 必须 == 语料里的 Debian hello（证明"装"是真装）；
        # ★ 这一批再收紧：**完整原样 xz 包**（49 条目 data.tar）的每个 payload 条目
        #   都要在卷上逐字节一致 —— 宿主侧用 Python 的 lzma/tarfile 独立解包比对。
        gnu = open(items["glibc_bin"]["host"], "rb").read()
        got = vol_read(tp, vol, "/usr/bin/hello")
        full = deb_payload_files(items["deb_real_xz"]["host"])
        bad = [p for p, b in sorted(full.items()) if vol_read(tp, vol, "/" + p) != b]
        check("⑩ vpkg 真把 Debian 的 /usr/bin/hello 写进了卷（%d B，与语料逐字节一致）；"
              "原样 xz 包的 %d 个 payload 条目也逐字节一致" % (len(gnu), len(full)),
              got == gnu and len(full) == 49 and not bad,
              ("卷上 %s；不一致 %s" % (len(got) if got is not None else "没找到", bad[:3])))
    except Exception as ex:                                    # noqa: BLE001
        check("⑩ 宿主侧卷解析跑通", False, str(ex)[:200])
    except Exception as ex:                                    # noqa: BLE001
        check("⑩ 宿主侧卷解析跑通", False, str(ex)[:200])

    # ---------- 证据与汇总 ----------
    print("--- 各类的内核串口原文（截断）---")
    for k in ("stat", "big", "dyn", "dyn_gnu", "longpath", "gnu", "deb_xz", "deb_run_full",
              "deb_dep", "deb_run"):
        if k in segs:
            print("   [%s] %s" % (k, raw(segs[k])))
    print("--- 缺口清单（FAIL 的断言 = 还没修的事）---")
    for name, c in checks:
        if not c:
            print("   [!] " + name)
    if args.keep:
        print("[linuxapp64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d fail=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _n, c in checks if c),
             sum(1 for _n, c in checks if not c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
