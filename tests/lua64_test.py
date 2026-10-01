#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/lua64_test.py - ★ A4-4a：**Ring 3 里的 Lua 5.4.7** 端到端验收

要证明的事（对应任务书 A4-4a 的五条）：
  ① `run /bin/lua -v` 打印 **Lua 5.4.7** 版本行（解释器真的在 ring3 跑起来了）；
  ② `-e` 算术后输出逐字节正确（shell 没有引号语法，所以代码写成"无空格 token"，见 TYPED）；
  ③ 跑一个**从系统卷读的脚本文件**（/tcc/demo/hello.lua）：算术 + 字符串 + for 循环，逐行断言；
  ④ `io.open`/`io.read` 能读**系统卷里的文本文件**（/etc/sh64hello.txt，内容构建期已知）——
     断言行内容 + 字节数（读文件走 musl 的 stdio -> 内核 open/read/close/fstat）；
  ⑤ 错误脚本给出 **文件名:行号 + 非 0 退出码**（不是 PANIC、不是静默成功）。

证据口径：终端里的每一行都走内核控制台（串口）。Lua 自己的输出与 shell 的 `run: … exited code=N`
都在**剥掉内核打点行**之后的"控制台流"里（见 console_stream 的说明）。
另外从夹具盘的卷里把 /lib/lua.bin、/bin/lua、/tcc/demo/*.lua 读回来做**字节级**比对。

用法：
    py -3 tests\\lua64_test.py
    py -3 tests\\lua64_test.py --img <已装好的盘> --timeout 240
退出码：0 = 硬断言全过；1 = 有硬断言失败；2 = 环境问题（缺构建产物 / 缺 qemu）。
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

# 构建产物默认在 build64/；`--build DIR` 可以指到"构建产物快照"目录（本仓库多个代理会在
# 同一时刻重建 build64，验收时用快照更稳）。
BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
SHELL_BIN = os.path.join(BUILD, "shell.bin")
KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
LUA_DRV = os.path.join(BUILD, "lua")
LUA_BIN = os.path.join(BUILD, "lua.bin")
TCC_DRV = os.path.join(BUILD, "tcc")
PROBE = os.path.join(BUILD, "a44probe")     # ★ A4-4b：两个新系统调用的 ring3 探针
TCC_BIN = os.path.join(BUILD, "tcc.bin")
TCC_STAGE = os.path.join(BUILD, "tcc_stage")
FIXTURE = os.path.join(BUILD, "lua_test.img")


def use_build_dir(path):
    """把上面那组产物路径改指到 path（--build）。"""
    global BUILD, SYSTEM_IMG, SHELL_BIN, KERNEL_OS, LUA_DRV, LUA_BIN, TCC_DRV, TCC_BIN, TCC_STAGE, FIXTURE, PROBE
    BUILD = path
    SYSTEM_IMG = os.path.join(BUILD, "system.img")
    SHELL_BIN = os.path.join(BUILD, "shell.bin")
    KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
    LUA_DRV = os.path.join(BUILD, "lua")
    LUA_BIN = os.path.join(BUILD, "lua.bin")
    TCC_DRV = os.path.join(BUILD, "tcc")
    TCC_BIN = os.path.join(BUILD, "tcc.bin")
    TCC_STAGE = os.path.join(BUILD, "tcc_stage")
    FIXTURE = os.path.join(BUILD, "lua_test.img")
    PROBE = os.path.join(BUILD, "a44probe")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512
MMAP_VA = 0x100000000 + 0x90000
BASE64 = 0x100000000
STACK64 = 0x100000000 + 0x10000

# 模板里的**已知内容**（构建期由 make_shellvol/tcc_pack 写入）——io 断言的期望值
HELLO_TXT = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"
DEMO_LUA = ("hello.lua", "io64.lua", "err.lua")

# shell 的 token 切分只认空格/tab（没有引号语法）——所以要敲的命令里不能有"引号内空格"。
# sendkey 名字表（缺的字符在这里补全；见 tests/tcc64_test.py 的同名表）
TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash", "(": "shift-9", ")": "shift-0",
    "+": "shift-equal", "*": "shift-8", "'": "apostrophe", '"': "shift-apostrophe",
    ",": "comma", ";": "semicolon", "[": "bracket_left", "]": "bracket_right",
    "{": "shift-bracket_left", "}": "shift-bracket_right", "%": "shift-5",
    "!": "shift-1", "#": "shift-3", "$": "shift-4", "&": "shift-7",
    "@": "shift-2", "^": "shift-6", "~": "shift-grave", "\\": "backslash",
}
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[LUADRV] FAIL",
    "[ELF64] reject",
    "selftest FAIL",
    "FAILED mask=",
]
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
        if os.sep in c or "/" in c:
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
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF"
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


def load_mod(name, fname):
    import importlib.util
    path = os.path.join(ROOT, "tools", fname)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_guest_file(img_path, parts):
    """从夹具盘的卷里读 /a/b/c（parts = ["tcc","demo","hello.lua"]）；找不到返回 None。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
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
        for i, nm, r in TP._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        _, rec = hit
        cur = hit[0]
    return TP._read_file(vol, rec, total)


def prepare_fixture():
    """用**与 build64.sh 同一套脚本**现造一块夹具盘（shell + tcc 基础卷 -> lua_pack 加 Lua）。"""
    for p in (SYSTEM_IMG, SHELL_BIN, TCC_DRV, TCC_BIN, LUA_DRV, LUA_BIN):
        if not os.path.exists(p):
            return None
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    LP = load_mod("lua_pack_win", "lua_pack_win.py")
    shell_bytes = open(SHELL_BIN, "rb").read()
    vol = TP.Volume2(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", HELLO_TXT, parent=etc_ino, mode=0o644)
    driver = open(TCC_DRV, "rb").read()
    tcc = open(TCC_BIN, "rb").read()
    vol.write_file("tcc", driver, parent=bin_ino, mode=0o755)
    lib_ino = vol.mkdir("lib", parent=0, mode=0o755)
    vol.write_file("tcc.bin", tcc, parent=lib_ino, mode=0o755)
    tcc_ino = vol.mkdir("tcc", parent=0, mode=0o755)
    tcc_demo_ino = vol.mkdir("demo", parent=tcc_ino, mode=0o755)
    expect = {"/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": HELLO_TXT,
              "/bin/tcc": driver, "/lib/tcc.bin": tcc}
    base = vol.finish()
    bad = TP.verify(base, expect)
    if bad:
        raise RuntimeError("夹具卷（基础）自检失败：%s" % bad)

    # 在基础卷上按 build64.sh 的第 2 步加 Lua（同一份 lua_pack_win.pack_into）+ A4-4b 探针
    drv = open(LUA_DRV, "rb").read()
    lb = open(LUA_BIN, "rb").read()
    ve = TP.VolumeEdit(TARGET_SECTORS - PART_MAIN_LBA)
    ve.load(base)
    probe = PROBE if os.path.exists(PROBE) else None
    lexpect = LP.pack_into(ve, drv, lb, os.path.join(ROOT, "user", "lua", "demo"), probe)
    # A4-4b 探针要用的素材目录：/tmp/sub 世界可写（跨目录 rename 的目标父目录）
    sub = ve.mkdirs("/tmp/sub", mode=0o777)
    if sub is None:
        raise RuntimeError("夹具卷：建不出 /tmp/sub")
    lua_vol = ve.finish()
    bad = TP.verify(lua_vol, lexpect)
    if bad:
        raise RuntimeError("夹具卷（Lua）自检失败：%s" % bad)
    img = TP.SV.build_disk(open(SYSTEM_IMG, "rb").read(), lua_vol, TARGET_SECTORS)
    with open(FIXTURE, "wb") as f:
        f.write(img)
    return FIXTURE, lexpect


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


def type_line(mon, text, per_key=0.12):
    for ch in text:
        if ch in TYPED_NAMES:
            mon.key(TYPED_NAMES[ch], wait=per_key)
        elif ch.isalnum():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r（加进 TYPED_NAMES）" % ch)
    mon.key("ret", wait=per_key + 0.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--no-desktop", action="store_true", help="只做交付/夹具断言（不启 QEMU）")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--build", default=None,
                    help="构建产物目录（默认 <repo>/build64；可指向一份产物快照，避免与并发构建打架）")
    args = ap.parse_args()
    if args.build:
        use_build_dir(os.path.abspath(args.build))

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    print("=== Vimtu64 A4-4a：Ring 3 Lua 5.4.7（/bin/lua + /lib/lua.bin + /tcc/demo/*.lua）acceptance ===")

    # ---------------- ① 交付：Lua 是"系统卷里的文件"，内核里搜不到 ----------------
    for p, what in ((LUA_DRV, "装载驱动 build64/lua"), (LUA_BIN, "解释器 build64/lua.bin")):
        check("构建产物存在：%s" % what, os.path.exists(p), p)
    if os.path.exists(LUA_DRV):
        d, etype, machine, entry, phoff, phes, phn, ph = elf_phdrs(LUA_DRV)
        loads = [x for x in ph if x[0] == 1]
        check("装载驱动 < 64 KiB 且 PT_LOAD 落在 4GiB..+64KiB（内核主程序装载器能装）",
              len(d) < 64 * 1024 and etype == 2 and
              all(x[2] >= BASE64 and x[2] + x[4] <= STACK64 for x in loads),
              "%d B entry=%#x phnum=%d" % (len(d), entry, phn))
        check("装载驱动无 PT_INTERP / PT_DYNAMIC", all(x[0] not in (2, 3) for x in ph))
    if os.path.exists(LUA_BIN):
        d, etype, machine, entry, phoff, phes, phn, ph = elf_phdrs(LUA_BIN)
        loads = [x for x in ph if x[0] == 1]
        check("lua.bin 是静态 ET_EXEC/x86_64、非 PIC 定址在 4GiB+0x90000",
              etype == 2 and machine == 0x3E and all(x[2] >= MMAP_VA for x in loads) and
              loads and loads[0][1] == 0 and loads[0][2] == MMAP_VA,
              "entry=%#x segs=%d" % (entry, len(loads)))
        check("lua.bin 无 PT_INTERP / PT_DYNAMIC（驱动不做重定位）", all(x[0] not in (2, 3) for x in ph))
    if os.path.exists(KERNEL_OS) and os.path.exists(LUA_BIN):
        k = open(KERNEL_OS, "rb").read()
        b = open(LUA_BIN, "rb").read()
        mid = len(b) // 2
        check("内核二进制里搜不到 lua.bin 的 64B 探针（Lua **不进内核镜像**，字节证据）",
              b[mid:mid + 64] not in k, "kernel64_os.bin=%d B lua.bin=%d B" % (len(k), len(b)))

    # 演示脚本的"期望输出"是**构建期已知**的（逐行断言，不是模糊匹配）
    want_arith = "arith: 1+2*3=7 sum(1..10)=55"
    want_str = "str:   vimtu len=7 upper=LUA"
    want_table = "table: 1,2,3 max=9"
    want_fmt = "fmt:   42/ok/2"

    strict = False
    if args.img:
        img = args.img
        if not os.path.exists(img):
            sys.stderr.write("镜像不存在：%s\n" % img)
            return 2
        print("[lua64] 直接用给定镜像：%s" % img)
    else:
        fx = prepare_fixture()
        if not fx:
            sys.stderr.write("缺少构建产物（先跑 bash build64.sh；它会调 tools/lua_build_win.sh）\n")
            return 2
        img, expect = fx
        strict = True
        check("夹具卷逐字节回读通过（/bin/lua、/lib/lua.bin、%d 个 .lua 演示脚本）" % len(DEMO_LUA),
              True, "img=%s" % os.path.basename(img))
        for p in ("/bin/lua", "/lib/lua.bin", "/tcc/demo/hello.lua",
                  "/tcc/demo/io64.lua", "/tcc/demo/err.lua"):
            check("卷里有 %s" % p, p in expect)
        # 从盘上把三个文件读回来做**字节级**比对（写完 -> 落盘 -> 再读回）
        for parts, src in ((["bin", "lua"], LUA_DRV), (["lib", "lua.bin"], LUA_BIN),
                           (["tcc", "demo", "hello.lua"], os.path.join(ROOT, "user", "lua", "demo", "hello.lua"))):
            got = read_guest_file(img, parts)
            want = open(src, "rb").read()
            check("从夹具盘卷里读回 /%s 与构建产物逐字节一致" % "/".join(parts), got == want,
                  "%s vs %s B" % (len(got or b""), len(want)))

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_lua64_")
    proc, mon, slog, serial = boot(qemu, img, "lua", tmp, args.timeout)
    log = slog()
    check("系统起来并进了桌面（[GUI64] ready）", "[GUI64] ready" in log, serial)

    def wait_raw(needle, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in slog():
                return True
            time.sleep(0.3)
        return False

    def wait_kernel_exit(raw_since, code, timeout=60):
        """等内核自己记录的"子进程退出码"（[PROC64] exit pid=N code=<c>）。

        为什么用它当退出码证据：shell 的 run 只调一次 wait4，而本内核对**还在跑**的子进程
        返回 -EAGAIN（已知缺陷，user/shell 与 kernel/proc64.* 都不在本批可改范围）—— 慢命令
        因此拿不到 `run: … exited code=N` 汇总行。内核日志是**更独立**的证据（谁都不能假装）。
        raw_since = 敲命令之前裸串口的字节长度（只认这之后的退出行）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if re.search(r"\[PROC64\] exit pid=\d+ code=%d" % code, slog()[raw_since:]) is not None:
                return True
            time.sleep(0.3)
        return False

    def wait_console(needle, timeout=30, since=0):
        """等"控制台输出流"（裸串口剥掉内核打点行）里出现 needle。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in shell_stream(slog()[since:]):
                return True
            time.sleep(0.3)
        return False

    opened = False
    for _ in range(3):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if "[APP] term opened" in slog():
            opened = True
            break
    check("桌面之后打开终端（开始菜单 -> 终端）", opened)
    type_line(mon, "shell", per_key=0.2)
    check("内核装载并启动 /bin/shell.bin", wait_raw("[SH64] launch path=/bin/shell.bin", 40))
    check("shell banner 出现", wait_raw("VimtuOS ring3 shell (sh64)", 30))
    time.sleep(1.0)

    # ---------------- ① -v：版本行 ----------------
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/lua -v")
    check("驱动把 /lib/lua.bin 读出来、按固定地址 mmap、改 auxv、jmp 进 Lua（[LUADRV] 打点）",
          wait_console("[LUADRV] load path=/lib/lua.bin", 60, since=base) and
          wait_console("[LUADRV] jmp entry=", 30, since=base))
    check("mmap 返回地址 = 4GiB+0x90000（Lua 的链接地址，驱动自己断言过）",
          "expected=0x100090000" in shell_stream(slog()[base:]) and
          "[LUADRV] FAIL reason=mmap-va-mismatch" not in shell_stream(slog()[base:]))
    check("驱动给 Lua 换了一块更大的栈（512 KiB；内核给的 16 KiB 不够）",
          "stack mmap va=" in shell_stream(slog()[base:]) and "size=524288" in shell_stream(slog()[base:]))
    check("★ Lua 版本行逐字节正确（Lua 5.4.7 在 ring3 里跑起来了）",
          wait_console("Lua 5.4.7  Copyright (C) 1994-2024 Lua.org, PUC-Rio", 90, since=base))
    check("★ 退出码证据：`run: /bin/lua … exited code=0` 或内核侧 `[PROC64] exit pid=… code=0`",
          (re.search(r"run: /bin/lua pid=\d+ exited code=0", shell_stream(slog()[base:])) is not None) or
          wait_kernel_exit(rb, 0, 60))

    # ---------------- ② -e 算术（引号口径见下面的收口修注释）----------------
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/lua -e print(1+2)")
    check("★ `-e` 算术输出逐字节 = 3", wait_console("\n3\n", 90, since=base))
    # ★ 收口修：带引号的 `-e` 必须**把整段用引号包住**再敲（正确引用写法）：
    #     run /bin/lua -e 'print("a".."b",10//3,2*10*10)'
    #   根因（实测）：交互行先过 user/shell/main.c 的 quote_fold。收口修之前它**任何** `'`/`"`
    #   都翻转引号状态，于是 `"a"`/`"b"` 的引号被折叠吃掉 -> Lua 收到 `print(a..b,…)` -> nil 拼接报错，
    #   断言红。现在 quote_fold **按引号种类配对**：外层单引号组里内的 `"` 是字面量，原样传给 Lua。
    #   口径（写清楚，避免下次再撞）：引号折叠**只对交互行**生效；`-c` 行（`/bin/sh -c '<cmd>'`）
    #   走 expand_cmd 的引号剥离，同样按种类配对。两条路径都在回归里：本行 + 下一行 = 交互行
    #   （两种引号种类各一遍），`-c` 路径由 make64_test 的 `sh -c` 子集（59 条 sh64 / 8 条 -c）覆盖。
    base2 = len(slog())
    type_line(mon, "run /bin/lua -e 'print(\"a\"..\"b\",10//3,2*10*10)'")
    check("★ `-e` 字符串/整除/乘法 = ab 3 200（单引号包整段：引号真的传给了 Lua）",
          wait_console("\nab\t3\t200\n", 90, since=base2))
    # 同一条命令的"镜像引用"：外层双引号、内层单引号 —— 证明两种引号种类都按字面量保留
    base2b = len(slog())
    type_line(mon, "run /bin/lua -e \"print('a'..'b',10//3,2*10*10)\"")
    check("★ 镜像引用：`-e \"print('a'..'b',…)\"` 同样 = ab 3 200（引号种类不影响字面量传递）",
          wait_console("\nab\t3\t200\n", 90, since=base2b))

    # ---------------- ③ 从系统卷读脚本文件并跑 ----------------
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/lua /tcc/demo/hello.lua")
    check("★ 脚本文件（/tcc/demo/hello.lua）的算术行逐字节正确", wait_console(want_arith, 90, since=base))
    check("★ 字符串行逐字节正确", wait_console(want_str, 30, since=base))
    check("★ 表的排序/最大值行逐字节正确", wait_console(want_table, 30, since=base))
    check("★ string.format 行逐字节正确", wait_console(want_fmt, 30, since=base))
    check("★ 脚本跑完的哨兵行 + 退出码 0（shell 汇总或内核 [PROC64] exit code=0）",
          wait_console("hello-lua-done", 30, since=base) and
          ((re.search(r"run: /bin/lua pid=\d+ exited code=0", shell_stream(slog()[base:])) is not None) or
           wait_kernel_exit(rb, 0, 60)))

    # ---------------- ④ io.open / io.read 读系统卷里的文本文件 ----------------
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/lua /tcc/demo/io64.lua")
    check("★ io.open(/etc/sh64hello.txt, \"r\") 成功（io-open-ok）", wait_console("io-open-ok /etc/sh64hello.txt", 90, since=base))
    check("★ io.read(\"l\") 读回的第一行与构建期写入的 /etc/sh64hello.txt 逐字节一致",
          wait_console("io-line1 " + HELLO_TXT.decode().rstrip("\n"), 30, since=base))
    want_rest = len(HELLO_TXT) - len(HELLO_TXT.split(b"\n")[0]) - 1
    check("★ io.read(\"a\") 读回的剩余字节数 = 文件长度 - 第一行（%d）" % want_rest,
          wait_console("io-reab %d" % want_rest, 30, since=base))
    check("★ io 脚本跑完 + 退出码 0（shell 汇总或内核 [PROC64] exit code=0）",
          wait_console("io64-done", 30, since=base) and
          ((re.search(r"run: /bin/lua pid=\d+ exited code=0", shell_stream(slog()[base:])) is not None) or
           wait_kernel_exit(rb, 0, 60)))

    # ---------------- ⑤ 错误脚本：文件名:行号 + 非 0 退出码（不是 PANIC）----------------
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/lua /tcc/demo/err.lua")
    check("★ 错误脚本给出 文件名:行号（/tcc/demo/err.lua:6: …），而不是 PANIC",
          wait_console("err.lua:6:", 90, since=base) and
          "attempt to index a nil value" in shell_stream(slog()[base:]))
    check("★ 错误脚本的退出码 = 1（shell 汇总或内核 [PROC64] exit code=1；不是 127 装载失败）",
          (re.search(r"run: /bin/lua pid=\d+ exited code=1", shell_stream(slog()[base:])) is not None) or
          wait_kernel_exit(rb, 1, 60),
          (re.search(r"run: /bin/lua[^\r\n]*", shell_stream(slog()[base:])) or ["（缺 shell 汇总行，用内核退出码判定）"])[0])
    check("错误脚本**没有**把 Lua 打成 PANIC/装载拒绝",
          "[PANIC" not in slog() and "[ELF64] reject" not in slog() and
          "[LUADRV] FAIL" not in slog())

    # ---------------- ⑥ ★ A4-4b：两个新系统调用的 ring3 探针（utime(132) / rename(82)）----------------
    # 为什么在这一节：a42a 那份既有的 A4-2a 探针断言"显式时间 = -ENOSYS、跨目录 = -ENOSYS"，
    # 而 A4-4b 把这两条实现了（旧断言过期，但 tests/a42a64_test.py 不在本批可改范围）——
    # 所以本批用 /bin/a44probe（user/lua/a44probe.c）独立取证：合法路径成功 + 非法路径错误码。
    base = len(slog())
    rb = len(slog())
    type_line(mon, "run /bin/a44probe")
    probe_want = [
        ("utime_null", "0", "utime(path, NULL) 幂等成功"),
        ("utime_explicit", "0", "utime 显式时间（1700000000 = 2023-11-14T22:13:20Z UTC）成功"),
        ("utime_missing", "-2", "utime 路径不存在 -> -ENOENT"),
        ("utime_badtime", "-22", "utime 时间超出 2000..2063 -> -EINVAL（不假装设成 1970）"),
        ("rename_same", "0", "rename 同目录改名成功"),
        ("rename_cross", "0", "★ rename **跨目录**移动成功（A4-4b 的新原语）"),
        ("rename_back", "0", "★ 再跨目录移回 /tmp 成功"),
        ("rename_missing", "-2", "rename 源不存在 -> -ENOENT"),
        ("rename_exists", "-17", "rename 目标已存在 -> -EEXIST（不覆盖）"),
        ("rename_parent_file", "-13", "rename 目标父目录是文件 -> -EACCES（vfs64 先命中『遍历要 x 位』；Linux 给 ENOTDIR，如实差异）"),
        ("rename_perm_denied", "-13", "rename 目标目录无写权限 -> -EACCES"),
        ("rename_dir_into_self", "-22", "★ 目录移进自己的子树 -> -EINVAL（Linux 口径）"),
        ("rmdir_dir", "0", "收尾：rmdir 成功"),
    ]
    got_probe = wait_console("A44 done=0", 90, since=base)
    for tag, val, why in probe_want:
        deadline = time.time() + 20
        got = None
        while time.time() < deadline:
            m = re.search(r"A44 %s=(-?\d+)" % re.escape(tag), shell_stream(slog()[base:]))
            if m:
                got = m.group(1)
                break
            time.sleep(0.3)
        check("探针 %-22s = %-4s  %s" % (tag, got if got is not None else "（缺）", why),
              got is not None and got == val)
    check("★ A4-4b 探针跑完（A44 done=0）+ 退出码 0",
          got_probe and
          ((re.search(r"run: /bin/a44probe pid=\d+ exited code=0", shell_stream(slog()[base:])) is not None) or
           wait_kernel_exit(rb, 0, 60)))
    check("★ 内核侧证据：两个号真的走了 syscall 指令表（[SYSCALL] insn nr=132 / nr=82）",
          "[SYSCALL] insn nr=132 " in slog()[rb:] and "[SYSCALL] insn nr=82 " in slog()[rb:])
    check("★ 内核侧证据：vfs64 的显式 mtime setter 与跨目录 rename 原语都落盘（[VFS64] utime ok / rename_to ok）",
          "[VFS64] utime ok idx=" in slog()[rb:] and "[VFS64] rename_to ok idx=" in slog()[rb:])
    check("A4-4b 探针没有把内核打成 PANIC",
          "[PANIC" not in slog()[rb:] and "[SYSCALL] deny" not in slog()[rb:])

    # ---------------- 逐条列出禁止项 ----------------
    for pat in FORBIDDEN:
        if pat in slog():
            check("全程无 %r" % pat, False, pat)
            break
    else:
        check("全程无 PANIC / [LUADRV] FAIL / 装载拒绝 / 自检失败", True)

    proc.kill()
    try:
        proc.wait(timeout=10)
    except Exception:
        pass
    failed = [(n, c) for n, c in checks if not c]
    print("=== RESULT: %s ===  checks=%d ok=%d failed=%d  log=%s" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c), len(failed), serial))
    if not args.keep and os.path.isdir(tmp):
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
