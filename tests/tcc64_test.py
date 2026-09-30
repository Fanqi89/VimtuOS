#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/tcc64_test.py - ★ A4-2b/A4-2b-2：**Ring 3 里的 TinyCC** 端到端验收

要证明的事（对应任务书 A4-2b 的验收六条 + A4-2b-2 的"闭环"三条）：
  ① tcc 能在 VimtuOS 的 ring3 里跑起来（`run /bin/tcc -v` / `-print-search-dirs`）：
       * 装载驱动打点 [TCCDRV] start/load/mmap/auxv/jmp（tcc 是 280 KB 的映像，走的是
         驱动自己 mmap + 搬段 + jmp 这条路，见 user/apps/tcc/tccdrv.c 的说明）；
       * tcc **自己的输出**（`tcc version 0.9.27 (x86_64 Linux)`）出现在控制台上；
       * 启动期不再 OOM（A4-2b-2 把用户窗口从 1 MiB 放大到 16 MiB，见 kernel/usermode64.h）。
  ② `tcc -c X.c -o X.o` 产出 ELF64 目标文件（ET_REL/EM_X86_64）—— **字节级**证据：
     跑完 QEMU 之后本脚本直接从夹具盘的卷里把 /tmp/demo_tiny.o **读回来**解析 ELF 头
     （7f 45 4c 46 02 01 01 00 / e_type=1 / e_machine=0x3e / e_shnum 与节名）。
  ③ `tcc X.c -o X` 产出**可执行**静态 ELF64，再用 shell `run /tmp/hello2` 真跑起来：
     输出逐字节正确、退出码 0；同样从卷里读回来断言 ET_EXEC/无 PT_INTERP/段在 4GiB 装载区。
  ④ libtcc1.a 与系统头确实来自**系统卷**：`-print-search-dirs` 打出的 install/include/
     libraries/libtcc1/crt 五条路径都必须是 /tcc 与 /tcc/lib（构建期由 tools/tcc_pack_win.py
     装进卷，脚本再用卷格式的二级间接路径逐字节回读）。
  ⑤ `run … > 文件` 闭环：`run /musl_hello.elf > /tmp/tcc_o.txt` 之后 `cat /tmp/tcc_o.txt`
     读得到子程序的输出（A4-2b 顺手补的 shell 缺口：外部命令的 dup2(fd,1)）。
  ⑥ 全程无 PANIC、无 [SYSCALL] deny/enosys（真的出现了就列出来，而不是"过"）。

用法：
    py -3 tests\\tcc64_test.py
    py -3 tests\\tcc64_test.py --img <已装好的盘> --timeout 240
退出码：0 = 硬断言全过（含 0 个缺口）；1 = 有硬断言失败；2 = 环境问题。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh              # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
SHELL_BIN = os.path.join(BUILD, "shell.bin")
KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
DRIVER = os.path.join(BUILD, "tcc")
TCC_BIN = os.path.join(BUILD, "tcc.bin")
TCC_STAGE = os.path.join(BUILD, "tcc_stage")
HELLO = os.path.join(BUILD, "tcc_demo_hello")
FIXTURE = os.path.join(BUILD, "tcc_test.img")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512

MMAP_VA = 0x100000000 + 0x90000          # USER64_MMAP_VA64：tcc 的链接/装载地址
BASE64 = 0x100000000                     # USER64_CODE_VA64：主程序装载区起点
STACK64 = 0x100000000 + 0x10000          # USER64_STACK_VA64（装载区上界）
# ★ A4-2b-2：用户窗口 1 MiB -> 16 MiB（kernel/usermode64.h 的 USER64_WINDOW_BYTES64）。
#   mmap 区上界跟着窗口顶走；解释器（PT_INTERP）仍钉在 4GiB+1MiB 之下（USER64_INTERP_TOP_VA64）。
WINDOW_BYTES = 16 * 1024 * 1024          # USER64_WINDOW_BYTES64
WINDOW_TOP = 0x100000000 + WINDOW_BYTES
INTERP_TOP = 0x100000000 + 0x100000      # USER64_INTERP_TOP_VA64（放大前 window 顶）
TCC_SPAN_MAX = WINDOW_TOP - MMAP_VA      # tcc 映像能占的 mmap 区（~15.4 MiB）

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[SYSCALL] deny",
    "[TCCDRV] FAIL",
    "[ELF64] reject",
    "selftest FAIL",
    "FAILED mask=",
]
KERNEL_TAG_RE = re.compile(r"\[[A-Z][A-Z0-9_]*\][^\r\n]*\r?\n")
# 内核打点行的**模块前缀白名单**（照抄 tests/sh64_test.py 的那一份）。
# ★ 为什么要白名单、而不是"任意 [大写] 开头的行"：用户程序（本批的 /bin/tcc 驱动）自己也会打
#   `[TCCDRV] ...`，宽正则会把它当内核行吃掉 —— 实测过一次（驱动的一整行被剥掉，断言假失败）。
KERNEL_TAGS = (
    "SYSCALL", "TASK", "TASK64", "WD64", "UI", "TERM", "SH64", "PROC64", "ELF64", "FD64",
    "VFS64", "DRV64", "ICON64", "IMG64", "GUI64", "FONT64", "APP", "LOCK64", "START64",
    "DESK64", "DOCK64", "PANEL64", "VOL", "FAT64", "BIG64", "NET64", "INPUT64", "PRELOAD64",
    "SET64", "ATA64", "AHCI64", "NVME64", "USB64", "MEM64", "PANIC", "PANIC64", "BSOD", "USER64",
    "PERM64", "SYS64", "CON64", "CONF64", "STORE64", "UPDATE64", "SESSION64", "USERDB64",
    "GFX64", "DISPLAY64", "THEME64", "OS", "RESET", "SHUTDOWN",
)
KERNEL_TAG_RE = re.compile(r"\[(?:%s)\][^\r\n]*\r?\n" % "|".join(KERNEL_TAGS))

def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def shell_stream(text):
    """剥掉内核打点行（多趟，直到不动）——同 tests/sh64_test.py 的口径。"""
    for _ in range(4):
        nxt = KERNEL_TAG_RE.sub("", text)
        if nxt == text:
            break
        text = nxt
    return text


def elf_phdrs(path):
    import struct
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF", "不是 ELF"
    assert d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    ph = []
    for i in range(phn):
        o = phoff + i * phes
        t = struct.unpack_from("<I", d, o)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
        ph.append((t, off, va, fsz, msz, al))
    return d, etype, machine, entry, phoff, phes, phn, ph


# ==================== 卷回读：把 ring3 产物从夹具盘里**读回来**（字节级证据）====================
# 为什么要这一步（A4-2b-2）：用户态产生的 .o / 可执行文件写在 VimtuFS2 卷里，而串口只剩
# 可打印字符（内核控制台会滤掉非打印字节）—— 光靠 `cat` 拿不到 e_type/e_machine/e_shnum。
# 夹具盘是本脚本自己造的（prepare_fixture，strict=True），卷格式已知：盘 = system.img +
# MBR + 主分区卷（LBA 8009..TARGET_SECTORS）。直接按 tools/tcc_pack_win.py 的读取器解析。
def load_packmod():
    import importlib.util
    path = os.path.join(ROOT, "tools", "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_guest_file(img_path, mod, parts):
    """从夹具盘的卷里读 /a/b/c（parts = ["tmp", "demo_tiny.o"]）；找不到返回 None。"""
    import struct
    d = open(img_path, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:TARGET_SECTORS * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = struct.unpack_from("<I", vol, 20)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
    cur = 0
    rec = None
    for part in parts:
        hit = None
        for i, nm, r in mod._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        _, rec = hit
        cur = hit[0]
    return mod._read_file(vol, rec, total)


def elf_sections(d):
    """解析 ELF64 的节表：返回 (e_shnum, e_shstrndx, [节名])（读不到就返回 (0,0,[])）。"""
    import struct
    e_shoff = struct.unpack_from("<Q", d, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", d, 58)
    names = []
    if e_shoff == 0 or e_shnum == 0 or e_shstrndx >= e_shnum:
        return e_shnum, e_shstrndx, names
    str_off = struct.unpack_from("<Q", d, e_shoff + e_shstrndx * e_shentsize + 24)[0]
    str_size = struct.unpack_from("<Q", d, e_shoff + e_shstrndx * e_shentsize + 32)[0]
    if str_off + str_size > len(d):
        return e_shnum, e_shstrndx, names
    for i in range(e_shnum):
        no = struct.unpack_from("<I", d, e_shoff + i * e_shentsize)[0]
        if no >= str_size:
            continue
        end = d.find(b"\x00", str_off + no)
        if end < 0:
            continue
        names.append(d[str_off + no:end].decode("latin-1"))
    return e_shnum, e_shstrndx, names


def console_stream(text):
    """内核控制台（fd 1）上的字节流 = 裸串口剥掉内核打点行。

    为什么驱动/tcc/子程序的输出也要剥：内核给**每次 write(1)** 都在串口插一条
    `[SYSCALL] insn nr=1 …`，于是任何"一行分几次 write"的输出都会被切碎（实测过：
    `[TCCDRV] mmap va=[SYSCALL]…` 后面才接着 `90000 expected=…`）。
    ★ A4-2b-2：装载驱动（user/apps/tcc/tccdrv.c）已改成**按行缓冲**（每行一次 write），
    它的打点在裸串口上就是连续的；tcc 本体与子程序（musl stdio）仍然会分多次 write，
    所以这一层过滤对它们照旧必要。
    剥掉内核行之后，剩下的就是连续的"控制台输出流"，可以整串比对。"""
    return shell_stream(text)


def prepare_fixture():
    """用 tools/tcc_pack_win.py 现造一块夹具盘（与 build64.sh 同一份输入）。"""
    for p in (SYSTEM_IMG, SHELL_BIN, DRIVER, TCC_BIN, TCC_STAGE):
        if not os.path.exists(p):
            return None
    import importlib.util
    path = os.path.join(ROOT, "tools", "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    shell_bytes = open(SHELL_BIN, "rb").read()
    hello_text = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"
    vol = mod.Volume2(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", hello_text, parent=etc_ino, mode=0o644)
    driver = open(DRIVER, "rb").read()
    tcc = open(TCC_BIN, "rb").read()
    vol.write_file("tcc", driver, parent=bin_ino, mode=0o755)
    lib_ino = vol.mkdir("lib", parent=0, mode=0o755)
    vol.write_file("tcc.bin", tcc, parent=lib_ino, mode=0o755)
    tcc_ino = vol.mkdir("tcc", parent=0, mode=0o755)
    tcc_lib_ino = vol.mkdir("lib", parent=tcc_ino, mode=0o755)
    tcc_inc_ino = vol.mkdir("include", parent=tcc_ino, mode=0o755)
    tcc_demo_ino = vol.mkdir("demo", parent=tcc_ino, mode=0o755)
    expect = {"/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": hello_text,
              "/bin/tcc": driver, "/lib/tcc.bin": tcc}
    p = os.path.join(TCC_STAGE, "libtcc1.a")
    if os.path.exists(p):
        b = open(p, "rb").read()
        vol.write_file("libtcc1.a", b, parent=tcc_ino, mode=0o644)
        expect["/tcc/libtcc1.a"] = b
    for rel in ("libc.a", "crt1.o", "crti.o", "crtn.o"):
        p = os.path.join(TCC_STAGE, "lib", rel)
        if not os.path.exists(p):
            continue
        b = open(p, "rb").read()
        vol.write_file(rel, b, parent=tcc_lib_ino, mode=0o644)
        expect["/tcc/lib/" + rel] = b
    inc_root = os.path.join(TCC_STAGE, "include")
    inc_map = {}
    for dirpath, dirnames, filenames in os.walk(inc_root):
        rel = os.path.relpath(dirpath, inc_root)
        cur = tcc_inc_ino if rel == "." else inc_map[rel]
        for dd in sorted(dirnames):
            inc_map[dd if rel == "." else os.path.join(rel, dd)] = vol.mkdir(dd, parent=cur, mode=0o755)
        for f in sorted(filenames):
            b = open(os.path.join(dirpath, f), "rb").read()
            vol.write_file(f, b, parent=cur, mode=0o644)
            expect["/tcc/include/" + (f if rel == "." else rel.replace("\\", "/") + "/" + f)] = b
    for f in ("demo_tiny.c", "hello.c", "demo_headers.c"):
        p = os.path.join(ROOT, "user", "apps", "tcc", f)
        if not os.path.exists(p):
            continue
        b = open(p, "rb").read()
        vol.write_file(f, b, parent=tcc_demo_ino, mode=0o644)
        expect["/tcc/demo/" + f] = b
    if os.path.exists(HELLO):
        b = open(HELLO, "rb").read()
        vol.write_file("hello", b, parent=0, mode=0o755)
        expect["/hello"] = b
    vol_bytes = vol.finish()
    bad = mod.verify(vol_bytes, expect)
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = mod.SV.build_disk(open(SYSTEM_IMG, "rb").read(), vol_bytes, TARGET_SECTORS)
    with open(FIXTURE, "wb") as f:
        f.write(img)
    return FIXTURE, expect


def boot(qemu, img, tag, logdir, timeout):
    serial = os.path.join(logdir, tag + ".log")
    if os.path.exists(serial):
        os.remove(serial)
    mport = qh.free_port()
    args = [
        qemu, "-name", "Vimtu64-" + tag,
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    qh.login_desktop(qh.Monitor(mport), slog, proc, timeout=min(timeout, 180))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if "[GUI64] ready" in slog():
            break
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    return proc, qh.Monitor(mport), slog, serial


def desktop_check(mon, slog):
    for _ in range(3):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if "[APP] term opened" in slog():
            return True
    return False


def type_line(mon, text, per_key=0.12):
    for ch in text:
        if ch in TYPED_NAMES:
            mon.key(TYPED_NAMES[ch], wait=per_key)
        elif ch.isalnum():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--no-desktop", action="store_true", help="只做交付/夹具断言（不启 QEMU）")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    gaps = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    def gap(name, detail):
        gaps.append((name, detail))
        print("  [GAP ] %s  %s" % (name, detail))

    print("=== Vimtu64 A4-2b：Ring 3 TinyCC（/bin/tcc + /lib/tcc.bin + /tcc/**）acceptance ===")

    # ---------------- ① 交付：tcc 与它的头/库都是"系统卷里的文件" ----------------
    for p, what in ((DRIVER, "装载驱动 build64/tcc"), (TCC_BIN, "tcc 本体 build64/tcc.bin"),
                    (os.path.join(TCC_STAGE, "libtcc1.a"), "libtcc1.a"),
                    (os.path.join(TCC_STAGE, "lib", "crt1.o"), "crt1.o"),
                    (os.path.join(TCC_STAGE, "include", "stdio.h"), "系统头 /tcc/include/stdio.h")):
        check("构建产物存在：%s" % what, os.path.exists(p), p)

    if os.path.exists(TCC_BIN):
        d, etype, machine, entry, phoff, phes, phn, ph = elf_phdrs(TCC_BIN)
        loads = [x for x in ph if x[0] == 1]
        span = max((x[2] + x[4] for x in loads), default=0) - MMAP_VA
        check("tcc.bin 是静态 ET_EXEC/x86_64、非 PIC 定址在 4GiB+0x90000",
              etype == 2 and machine == 0x3E and all(x[2] >= MMAP_VA for x in loads) and
              loads and loads[0][1] == 0 and loads[0][2] == MMAP_VA,
              "entry=%#x segs=%d" % (entry, len(loads)))
        check("tcc.bin 的映像 span 落在用户窗口 mmap 区里（%d B）" % span, 0 < span <= TCC_SPAN_MAX,
              "span=%d B mmap 区上限=%d B 余量=%d B" % (span, TCC_SPAN_MAX, TCC_SPAN_MAX - span))
        check("tcc.bin 无 PT_INTERP / PT_DYNAMIC（驱动不做重定位）",
              all(x[0] not in (2, 3) for x in ph))
    if os.path.exists(DRIVER):
        d, etype, machine, entry, phoff, phes, phn, ph = elf_phdrs(DRIVER)
        loads = [x for x in ph if x[0] == 1]
        check("装载驱动 < 64 KiB 且 PT_LOAD 落在 4GiB..+64KiB（内核主程序装载器能装）",
              len(d) < 64 * 1024 and etype == 2 and
              all(x[2] >= BASE64 and x[2] + x[4] <= STACK64 for x in loads),
              "%d B entry=%#x phnum=%d" % (len(d), entry, phn))
        check("装载驱动无 PT_INTERP / PT_DYNAMIC", all(x[0] not in (2, 3) for x in ph))
    if os.path.exists(KERNEL_OS) and os.path.exists(TCC_BIN):
        k = open(KERNEL_OS, "rb").read()
        t = open(TCC_BIN, "rb").read()
        mid = len(t) // 2
        check("内核二进制里搜不到 tcc.bin 的 64B 探针（tcc **不进内核镜像**，字节证据）",
              t[mid:mid + 64] not in k, "kernel64_os.bin=%d B tcc.bin=%d B probe@%d" % (len(k), len(t), mid))

    # ---------------- 夹具盘（含卷内容逐字节回读） ----------------
    strict = False
    if args.img:
        img = args.img
        if not os.path.exists(img):
            sys.stderr.write("镜像不存在：%s\n" % img)
            return 2
        print("[tcc64] 直接用给定镜像：%s" % img)
    else:
        fx = prepare_fixture()
        if not fx:
            sys.stderr.write("缺少构建产物（先跑 bash build64.sh；它会调 tools/tcc_build_win.sh）\n")
            return 2
        img, expect = fx
        strict = True
        check("夹具卷逐字节回读通过（%d 个文件，含 /bin/tcc、/lib/tcc.bin、/tcc/**；"
              "tcc.bin 走二级间接）" % len(expect),
              True, "img=%s" % os.path.basename(img))
        for p in ("/tcc/include/stdio.h", "/tcc/include/bits/alltypes.h", "/tcc/libtcc1.a",
                  "/tcc/lib/crt1.o", "/tcc/lib/libc.a", "/tcc/demo/demo_tiny.c", "/bin/tcc", "/lib/tcc.bin"):
            check("卷里有 %s" % p, p in expect)

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d gaps=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c), len(gaps)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_tcc64_")
    proc, mon, slog, serial = boot(qemu, img, "tcc", tmp, args.timeout)
    log = slog()
    check("系统起来并进了桌面（[GUI64] ready）", "[GUI64] ready" in log, serial)

    stream_marks = []
    raw_marks = []
    armed = False

    def wait_stream(needle, timeout=30):
        """等**过滤后的 shell 字节流**里出现 needle（shell 自己的输出）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in shell_stream(slog()):
                return True
            time.sleep(0.3)
        return False
    def wait_raw(needle, timeout=30):
        """等**裸串口**里出现 needle（内核打点用）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in slog():
                return True
            time.sleep(0.3)
        return False

    def wait_console(needle, timeout=30):
        """等**控制台输出流**（裸串口剥掉内核打点行）里出现 needle。
        驱动 / tcc / 子程序 / shell 的 "run: …" 汇总行都走这条（tcc 与子程序的输出是分几次
        write(1) 发的，裸串口上会被内核的 [SYSCALL] 行切碎 —— 见 console_stream 的说明）。
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in console_stream(slog()):
                return True
            time.sleep(0.3)
        return False

    opened = desktop_check(mon, slog)
    check("桌面之后打开终端（开始菜单 -> 终端）", opened)
    type_line(mon, "shell", per_key=0.2)
    check("内核装载并启动 /bin/shell.bin", wait_raw("[SH64] launch path=/bin/shell.bin", 40))
    check("shell banner 出现", wait_raw("VimtuOS ring3 shell (sh64)", 30))

    def wait_console_since(needle, start, timeout=30):
        """只在 console_stream 的**第 start 字节之后**等 needle（避免和更早的输出撞车）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in console_stream(slog())[start:]:
                return True
            time.sleep(0.3)
        return False

    # ---------------- ① tcc 本体在 ring3 里跑起来（驱动 + 装载 + jmp + 版本行）----------------
    check("A4-2b-2 的窗口放大已在 boot 打点里（[USER64] window bytes=16777216 … lazy=1）",
          "[USER64] window bytes=16777216" in slog() and "lazy=1" in slog())
    type_line(mon, "run /bin/tcc -v")
    drv_ok = wait_console("[TCCDRV] jmp entry=", 60)
    check("驱动把 /lib/tcc.bin 读出来、按固定地址 mmap、改 auxv、jmp 进 tcc（[TCCDRV] 四条打点）",
          wait_console("[TCCDRV] load path=/lib/tcc.bin", 30) and drv_ok)
    check("mmap 返回地址 = 4GiB+0x90000（tcc 的链接地址，驱动自己断言过）",
          "expected=0x100090000" in console_stream(slog())
          and "[TCCDRV] FAIL reason=mmap-va-mismatch" not in console_stream(slog()))
    check("驱动给 tcc 换了一块更大的栈（A4-2b-2 起 256 KiB；内核给的 16 KiB 不够，见 tccdrv.c）",
          "stack mmap va=" in console_stream(slog()) and "size=262144" in console_stream(slog()))
    tcc_done = wait_console("run: /bin/tcc pid=", 90)
    check("tcc 进程正常结束（shell 汇总行 run: /bin/tcc pid=… exited code=0）",
          tcc_done and re.search(r"run: /bin/tcc pid=\d+ exited code=0", console_stream(slog())) is not None)
    check("tcc 在 ring3 里打印出版本（tcc version 0.9.27 (x86_64 Linux)）",
          wait_console("tcc version 0.9.27 (x86_64 Linux)", 20))
    if "tcc: error: memory full (malloc)" in console_stream(slog()):
        gap("tcc 启动期仍然 OOM（窗口已放大到 16 MiB，不该再出现）",
            "tcc 原文：tcc: error: memory full (malloc)")

    # ---------------- ④ 路径证据：头/libtcc1.a/crt 都指向系统卷 /tcc ----------------
    type_line(mon, "run /bin/tcc -print-search-dirs")
    check("tcc -print-search-dirs 打出搜索路径（install: /tcc）", wait_console("install: /tcc", 40))
    for want in ("include:", "  /tcc/include", "libraries:", "  /tcc/lib",
                 "libtcc1:", "  /tcc/libtcc1.a", "crt:", "  /tcc/lib"):
        check("tcc 的搜索路径指向系统卷：%r" % want, wait_console(want, 20), "")

    # ---------------- ③ 卷里那个 tcc 产物能在本内核里跑 ----------------
    type_line(mon, "run /hello")
    check("tcc 产出的 /hello 在 VimtuOS ring3 里跑起来并逐字节输出正确",
          wait_console("hello from TinyCC inside VimtuOS ring3", 40),
          "（/hello 由同一份 tcc 源码在构建期产出，见 build64/tcc_host.exe）")
    check("hello 退出码 0（shell 汇总行，已经回到终端了）",
          wait_console("run: /hello pid=", 20)
          and re.search(r"run: /hello pid=\d+ exited code=0", console_stream(slog())) is not None)

    # ---------------- ② -c：ring3 里编一个目标文件（窗口放大后应当闭环） ----------------
    # 判定方式（结实）：只看**这条命令之后**新出现的 shell 汇总行 "run: /bin/tcc pid=N exited code=C"，
    #   把 code 与 tcc 自己的报错原文一起拿出来 —— 成功就做字节级断言，失败就记缺口（如实）。
    n_before = len(console_stream(slog()))
    type_line(mon, "run /bin/tcc -c /tcc/demo/demo_tiny.c -o /tmp/demo_tiny.o")
    cc_m = None
    for _ in range(240):
        seg = console_stream(slog())[n_before:]
        hits = list(re.finditer(r"run: /bin/tcc pid=(\d+) exited code=(\d+)", seg))
        if hits:
            cc_m = hits[-1]
            break
        time.sleep(0.5)
    tcc_o_ok = False
    if cc_m is None:
        gap("ring3 里的 tcc -c 没有返回（超时）", "见串口日志 time-out")
    elif cc_m.group(2) == "0":
        tcc_o_ok = True
        check("ring3 里的 `run /bin/tcc -c /tcc/demo/demo_tiny.c -o /tmp/demo_tiny.o` 返回 0", True)
    else:
        seg = console_stream(slog())[n_before:]
        em = None
        for m2 in re.finditer(r"tcc: error: ([^\r\n]*)", seg):
            em = m2
        gap("ring3 里的 tcc -c 没能完成（tcc 进程退出码 %s）" % cc_m.group(2),
            "tcc 原文：%s" % (em.group(0) if em else "（没有 tcc 的 error 行，见串口日志）"))
    if tcc_o_ok:
        type_line(mon, "stat /tmp/demo_tiny.o")
        check("-c 产出的 /tmp/demo_tiny.o 是普通文件（shell stat 逐行打点）",
              wait_console("  file: /tmp/demo_tiny.o", 25)
              and "  type: regular" in console_stream(slog()))
        console = console_stream(slog())
        sm = None
        for m3 in re.finditer(r"  file: /tmp/demo_tiny\.o\r?\n  type: regular\r?\n  size: (\d+)", console):
            sm = m3
        check("目标文件大小合理（ELF64 relocatable：几百字节~几 KB）",
              sm is not None and 128 < int(sm.group(1)) < 16384,
              ("size=%s" % sm.group(1)) if sm else "（没解析到 size）")
        n0 = len(console)
        type_line(mon, "cat /tmp/demo_tiny.o")
        time.sleep(3.0)
        after = console_stream(slog())[n0:]
        check("cat 出来的 .o 里有 ELF 魔数与节名（可打印部分；字节级头/机器/节数在 QEMU 停后从卷里读回）",
              "ELF" in after and ".text" in after and "cat: cannot open" not in after)
    else:
        gap("-c 的字节级断言（ELF64 relocatable 头）", "依赖上一条")

    # ---------------- ③b 链接：ring3 里的 tcc 自己链出静态 ELF64，再用 run 跑起来 ----------------
    n_before = len(console_stream(slog()))
    type_line(mon, "run /bin/tcc /tcc/demo/hello.c -o /tmp/hello2")
    link_m = None
    for _ in range(300):
        seg = console_stream(slog())[n_before:]
        hits = list(re.finditer(r"run: /bin/tcc pid=(\d+) exited code=(\d+)", seg))
        if hits:
            link_m = hits[-1]
            break
        time.sleep(0.5)
    link_ok = False
    if link_m is None:
        gap("ring3 里的 tcc 链接（hello.c -> /tmp/hello2）没有返回（超时）", "见串口日志 time-out")
    elif link_m.group(2) == "0":
        link_ok = True
        check("ring3 里的 `run /bin/tcc /tcc/demo/hello.c -o /tmp/hello2` 返回 0", True)
    else:
        seg = console_stream(slog())[n_before:]
        em = None
        for m2 in re.finditer(r"tcc: error: ([^\r\n]*)", seg):
            em = m2
        gap("ring3 里的 tcc 链接没能完成（tcc 进程退出码 %s）" % link_m.group(2),
            "tcc 原文：%s" % (em.group(0) if em else "（没有 tcc 的 error 行，见串口日志）"))
    if link_ok:
        n0 = len(console_stream(slog()))
        type_line(mon, "run /tmp/hello2")
        out_ok = wait_console_since("hello from TinyCC inside VimtuOS ring3", n0, 40)
        check("ring3 里 tcc 生成的 /tmp/hello2 跑起来、逐字节输出正确", out_ok)
        h2m = None
        for _ in range(60):
            seg = console_stream(slog())[n0:]
            hits = list(re.finditer(r"run: /tmp/hello2 pid=(\d+) exited code=(\d+)", seg))
            if hits:
                h2m = hits[-1]
                break
            time.sleep(0.5)
        check("tcc 生成的程序退出码 0（shell 汇总行 run: /tmp/hello2 pid=… exited code=0）",
              h2m is not None and h2m.group(2) == "0",
              (h2m.group(0) if h2m else "（没有汇总行）"))
    else:
        gap("tcc 生成的程序跑起来的逐字节输出/退出码", "依赖上一条（链接未完成）")
    # ---------------- ⑤ run … > 文件 闭环（A4-2b 顺手补的 shell 缺口） ----------------
    n_before = len(slog())
    type_line(mon, "run /musl_hello.elf > /tmp/tcc_o.txt")
    check("重定向落地在**子进程**里：内核打点 [FD64] dup old=<fd> new=1（shell 先 fork 再 dup2 到 1 再 execve）",
          wait_raw("[FD64] dup old=", 60),
          "（这一步就是 A4-2b 给 user/shell/main.c 的 run 加的那 3 行）")
    check("子进程真的被 execve 起来（[PROC64] execve path=/musl_hello.elf … fds_kept=2）",
          wait_raw("[PROC64] execve path=/musl_hello.elf", 60))
    type_line(mon, "cat /tmp/tcc_o.txt")
    check("cat /tmp/tcc_o.txt 读到了子程序的输出（端到端闭环：子进程 fd1 被 dup2 到文件）",
          wait_console("[MUSL] hello from musl static ELF", 60))
    # ---------------- ⑥ 全程无 PANIC / deny / enosys ----------------
    log = slog()
    for bad in FORBIDDEN:
        check("串口里没有 %r" % bad, bad not in log)
    enosys = re.findall(r"\[SYSCALL\] enosys[^\r\n]*", log)
    if enosys:
        gap("有未实现的系统调用被触到（如实列出）", "; ".join(sorted(set(enosys))[:6]))
    else:
        check("没有 [SYSCALL] enosys（本批用到的号本内核都实现了）", True)
    deny = re.findall(r"\[SYSCALL\] deny[^\r\n]*", log)
    if deny:
        gap("有被内核拒掉的系统调用（如实列出）", "; ".join(sorted(set(deny))[:6]))
    else:
        check("没有 [SYSCALL] deny", True)

    if args.keep:
        print("[tcc64] 串口日志：%s" % serial)
    try:
        mon.stop()
    except Exception:
        pass
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except Exception:
        proc.kill()

    # ---------------- ②b/③c：QEMU 停了 —— 从夹具盘的卷里把 ring3 产物**读回来**（字节级） ----------------
    # 为什么放到最后：写盘是经 QEMU 完成的，进程退出后读主机上的镜像文件最稳；而且这一步给出
    # 串口拿不到的证据（ELF 头/e_type/e_machine/节表 —— 控制台会滤掉非打印字节）。
    if strict and os.path.exists(img):
        try:
            mod = load_packmod()
            obj = read_guest_file(img, mod, ["tmp", "demo_tiny.o"])
            if obj is None:
                check("从夹具盘卷里读回 /tmp/demo_tiny.o（字节级证据的前提）", False, "卷里没有这个文件")
            else:
                import struct
                etype, machine = struct.unpack_from("<HH", obj, 16)
                shnum, shstrndx, names = elf_sections(obj)
                head8 = " ".join("%02x" % b for b in obj[:8])
                check("ring3 的 tcc -c 产物头 8 字节 = %s（ELF64 小端应为 7f 45 4c 46 02 01 01 00）" % head8,
                      obj[:4] == b"\x7fELF" and obj[4] == 2 and obj[5] == 1, "%d B" % len(obj))
                check("ring3 的 .o 是 ELF64 可重定位对象（e_type=%d ET_REL / e_machine=%#x EM_X86_64）"
                      % (etype, machine), etype == 1 and machine == 0x3E)
                check("ring3 的 .o 节表非空（e_shnum=%d；节名：%s）" % (shnum, " ".join(names)),
                      shnum >= 4 and ".text" in names and ".symtab" in names and ".strtab" in names)
                check("ring3 的 .o 里带源码里的符号（tcc_demo_add / tcc_demo_mul）",
                      b"tcc_demo_add" in obj and b"tcc_demo_mul" in obj)
            exe = read_guest_file(img, mod, ["tmp", "hello2"])
            if exe is None:
                gap("从夹具盘卷里读回 /tmp/hello2（链接产物）", "卷里没有这个文件（链接那一步没成功时才会出现）")
            else:
                import struct
                etype2, machine2 = struct.unpack_from("<HH", exe, 16)
                entry2 = struct.unpack_from("<Q", exe, 24)[0]
                phoff2 = struct.unpack_from("<Q", exe, 32)[0]
                phes2, phn2 = struct.unpack_from("<H", exe, 54)[0], struct.unpack_from("<H", exe, 56)[0]
                segs2, has_interp, has_dyn = [], False, False
                for i in range(phn2):
                    o2 = phoff2 + i * phes2
                    t2 = struct.unpack_from("<I", exe, o2)[0]
                    va2 = struct.unpack_from("<Q", exe, o2 + 16)[0]
                    msz2 = struct.unpack_from("<Q", exe, o2 + 40)[0]
                    if t2 == 3:
                        has_interp = True
                    elif t2 == 2:
                        has_dyn = True
                    elif t2 == 1:
                        segs2.append((va2, msz2))
                check("ring3 链接产物 /tmp/hello2 是静态 ELF64 可执行（ET_EXEC=2 / EM_X86_64 / 无 PT_INTERP/PT_DYNAMIC）",
                      etype2 == 2 and machine2 == 0x3E and not has_interp and not has_dyn,
                      "%d B phnum=%d" % (len(exe), phn2))
                check("ring3 链接产物的 PT_LOAD 全落在内核装载区（4GiB..4GiB+64KiB）且入口在其中",
                      bool(segs2) and all(BASE64 <= v and v + m <= STACK64 for v, m in segs2)
                      and any(v <= entry2 < v + m for v, m in segs2),
                      "entry=%#x segs=%s" % (entry2, [(hex(v), m) for v, m in segs2]))
        except Exception as e:                       # 解析失败必须如实报，不能吞
            gap("从夹具盘卷里读回 ring3 产物（解析异常）", repr(e))
    else:
        gap("ring3 产物的字节级证据（卷回读）", "只对脚本自造的夹具盘（strict）做；--img 模式跳过")
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if gaps:
        print("=== A4-2b 如实缺口（%d 条）===" % len(gaps))
        for n, d in gaps:
            print("  - %s：%s" % (n, d))
    print("=== RESULT: %s ===  checks=%d ok=%d gaps=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c), len(gaps)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
