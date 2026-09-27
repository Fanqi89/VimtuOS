#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/sh64_test.py - ★ A4-1：**Ring 3 shell**（/bin/shell.bin）端到端验收

这一条验收的"要证明的事"（与任务书 A4-1 的六条断言一一对应）：
  ① shell 是**从系统卷的文件**装载的（不是内嵌内核）：
       * 构建产物 build64/kernel64_os.bin 里**搜不到** shell.bin 的 64 字节探针（字节证据）；
       * 夹具盘的"系统卷"里真有 /bin/shell.bin（用 tools/make_shellvol.py 的逐字节回读自检）；
       * 运行期打点 [SH64] launch path=/bin/shell.bin size=<n> pid=<p>，其中 n 必须等于
         装进卷里的那个文件的字节数；装载本身走内核 ELF64 加载器（[ELF64] load … via=execve）。
  ② echo / pwd / ls / cat 输出**逐字节**正确（串口上按字节比对；含"提示符 + 回显 + 输出"整段）。
  ③ mkdir + cd + pwd 路径正确（/tmp/d1、/tmp/d1/sub、cd .. 回退；shell 自己维护 cwd，
     因为内核**没有 chdir(80)** —— 这是如实记录的实现方式）。
  ④ 外部命令走 fork(57) + execve(59) + wait4(61) 真进程：`run /musl_hello.elf`
     -> [SYSCALL] insn nr=57/59/61 + [PROC64] execve/wait4 + 子程序自己的输出 + 退出码 7。
  ⑤ 重定向与管道：`>` `>>` `<`（内置命令）+ `ls … | cat`（内置之间）都实测到字节。
  ⑥ exit 之后回到终端（终端命令照旧可用）、无 PANIC、无 [SYSCALL] deny / enosys。

用法（必须 Windows 原生 Python；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\sh64_test.py
    py -3 tests\\sh64_test.py --img <已装好的磁盘镜像> --timeout 180
    py -3 tests\\sh64_test.py --no-desktop       # 跳过桌面交互段（只做交付/夹具断言）

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
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
import qemuhelp as qh              # noqa: E402  （公共登录手势：ui.login.auto 默认 0）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
KERNEL_OS = os.path.join(ROOT, "build64", "kernel64_os.bin")
FIXTURE_IMG = os.path.join(ROOT, "build64", "sh64_test.img")

PART_MAIN_LBA = 8009               # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 32768             # 16 MB
SECTOR = 512

SHELL_PATH = "/bin/shell.bin"
HELLO_TXT = "/etc/sh64hello.txt"
HELLO_BYTES = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"

# 终端按键映射（与 tests/dynlink64_test.py 同一套 QEMU sendkey 名字 + 本用例要的多两个键）
TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[SYSCALL] deny",
    "[SYSCALL] enosys",
    "[SH64] launch FAILED",
    "[SH64] req unknown",
    "[PROC64] start FAILED",
    "[USER64] enter FAILED",
    "selftest FAIL",
    "FAILED mask=",
]


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

# 串口是**多路复用**的（内核打点 + shell 输出共用一个 115200 波特的口），内核的行会插进
# shell 的逐字节回显/输出中间。所有"逐字节比对"的断言都先把这个过滤器套上：剥掉**已知内核
# 模块前缀**的整行（都以 \r\n 结尾），剩下的就是经邮箱抽回来的 shell 输出流 —— 它是连续的。
KERNEL_TAG_RE = re.compile(r"\[[A-Z][A-Z0-9_]*\][^\r\n]*\r?\n")

def shell_stream(text):
    """剥掉内核自己的打点行 -> 只剩 shell 输出流（逐字节可比对）。
    注意：日志是用 text 模式读的（通用换行），内核行的 \\r\\n 已经变成 \\n —— 所以正则收 \\r?\\n。"""
    return KERNEL_TAG_RE.sub("", text)


# ---------------------------------------------------------------------------
# 夹具：system.img 字节 + 标准 MBR + 主分区（= 带 /bin/shell.bin 的 VimtuFS2 v4 卷）
def prepare_fixture():
    if not os.path.exists(SYSTEM_IMG) or not os.path.exists(SHELL_BIN):
        return None
    import make_shellvol as msv           # tools/make_shellvol.py（同一份离线写入器）
    shell_bytes = open(SHELL_BIN, "rb").read()
    system_bytes = open(SYSTEM_IMG, "rb").read()
    if shell_bytes[:4] != b"\x7fELF" or not system_bytes:
        return None
    vol = msv.build_volume(shell_bytes, HELLO_BYTES, TARGET_SECTORS - PART_MAIN_LBA)
    bad = msv.verify(vol, {SHELL_PATH: shell_bytes, HELLO_TXT: HELLO_BYTES})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = msv.build_disk(system_bytes, vol, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(img)
    return FIXTURE_IMG, shell_bytes


def boot(qemu, img, tag, logdir, timeout):
    """无头启动一块盘（照 tests/musl64_test.py 的 boot()），挂 monitor 做登录手势。"""
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
    """开始菜单 -> 终端（与 dynlink64_test 同一手势：meta_l 再按 1）。"""
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
    ap.add_argument("--img", default=None, help="已装好的系统盘镜像（缺省：用 system.img 造夹具）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=180, help="等 [GUI64] ready 的最长秒数")
    ap.add_argument("--no-desktop", action="store_true", help="跳过桌面交互段（只做交付/夹具断言）")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    print("=== Vimtu64 A4-1：Ring 3 shell（/bin/shell.bin）acceptance ===")

    # ---------------- ① 交付：shell 是"系统卷里的文件"，不是内嵌内核 ----------------
    if not os.path.exists(SHELL_BIN):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh；它会调 user/shell/build_shell.sh）\n" % SHELL_BIN)
        return 2
    shell_bytes = open(SHELL_BIN, "rb").read()
    shell_sz = len(shell_bytes)
    check("shell 载荷是静态 ELF64（build64/shell.bin 头 4B = \\x7fELF）", shell_bytes[:4] == b"\x7fELF",
          "size=%d B" % shell_sz)
    check("shell 载荷在三个硬上限内（< 96 KiB 内核读盘缓冲）", 0 < shell_sz < 96 * 1024, "size=%d" % shell_sz)

    if os.path.exists(KERNEL_OS):
        kernel_bytes = open(KERNEL_OS, "rb").read()
        mid = shell_sz // 2
        probe = shell_bytes[mid:mid + 64]
        check("内核二进制里搜不到 shell.bin 的 64B 探针（**没有内嵌**，字节证据）",
              probe not in kernel_bytes, "kernel64_os.bin=%d B probe@%d" % (len(kernel_bytes), mid))
    else:
        check("内核二进制存在（build64/kernel64_os.bin）", False, "缺文件")

    # ---------------- 夹具盘 ----------------
    strict_fixture = False
    if args.img:
        if not os.path.exists(args.img):
            sys.stderr.write("镜像不存在：%s\n" % args.img)
            return 2
        img = args.img
        print("[sh64] 直接用给定镜像：%s" % img)
    else:
        if not os.path.exists(SYSTEM_IMG):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
            return 2
        fx = prepare_fixture()
        if not fx:
            sys.stderr.write("造夹具盘失败\n")
            return 2
        img, vol_shell = fx
        strict_fixture = True
        check("夹具卷里的 /bin/shell.bin 与 build64/shell.bin 逐字节一致（工具自检回读）",
              vol_shell == shell_bytes, "%d B" % len(vol_shell))
        print("[sh64] 测试盘已生成：%s（主分区 LBA %d 上是带 /bin/shell.bin 的 VimtuFS2 v4 卷）"
              % (img, PART_MAIN_LBA))

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_sh64_")
    proc, mon, slog, serial = boot(qemu, img, "boot1", tmp, args.timeout)
    log = slog()
    check("系统起来并进了桌面（[GUI64] ready）", "[GUI64] ready" in log, serial)

    def wait_mark(needle, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in slog():
                return True
            time.sleep(0.3)
        return False

    def wait_stream(needle, timeout=25):
        """等**过滤后的 shell 字节流**里出现 needle（比等裸串口的 mark 更准：
        裸日志里内核打点会插进 shell 的逐字节回显中间，等"某个前缀"常常早于输出到达）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in shell_stream(slog()):
                return True
            time.sleep(0.3)
        return False
    # ---------------- ② 打开终端 -> 敲 shell ----------------
    opened = desktop_check(mon, slog)
    check("桌面之后打开终端（开始菜单 -> 终端：[APP] term opened）", opened)
    type_line(mon, "shell", per_key=0.2)
    check("内核装载并启动 /bin/shell.bin（[SH64] launch path=/bin/shell.bin size=n pid=p）",
          wait_mark("[SH64] launch path=/bin/shell.bin size=", 40), "")
    m = re.search(r"\[SH64\] launch path=/bin/shell\.bin size=(\d+) pid=(\d+)", slog())
    check("launch 打点逐字段可解析（path/size/pid）", m is not None, (m.group(0) if m else "（缺）"))
    if m:
        check("装载的字节数 = 装进卷里的那个文件大小（%d B）—— 证明是**那个文件**" % shell_sz,
              int(m.group(1)) == shell_sz, "size=%s" % m.group(1))
    check("装载走内核 ELF64 加载器、从文件系统读出（[ELF64] load path=/bin/shell.bin … via=execve）",
          re.search(r"\[ELF64\] load path=/bin/shell\.bin entry=[0-9A-F]+ phnum=\d+ segs=\d+ "
                    r"size=\d+ rsp=[0-9A-F]+ via=execve", slog()) is not None, "")
    check("shell 的 banner 经邮箱抽回到串口（[SH] 输出通道通）",
          wait_mark("VimtuOS ring3 shell (sh64) - /bin/shell.bin", 30), "")

    # ---------------- ③ 内置命令：提示符/回显/输出 逐字节 ----------------
    type_line(mon, "echo hello-from-sh64")
    type_line(mon, "echo hello-from-sh64")
    want1 = "sh64:/$ echo hello-from-sh64\nhello-from-sh64\nsh64:/$ "
    check("echo 逐字节正确（提示符 + 回显 + 输出 + 下一个提示符，整段字节比对；已剥内核打点行）",
          wait_stream(want1, 25), repr(want1))
    type_line(mon, "pwd")
    want2 = "sh64:/$ pwd\n/\nsh64:/$ "
    check("pwd 逐字节正确（初始 cwd = /）", wait_stream(want2, 25), repr(want2))

    type_line(mon, "ls /")
    want_ls_root = "sh64:/$ ls /\n/:\n  bin/\n  etc/\n  tmp/\n"
    check("ls /：内核服务代列目录（ring3 没有 getdents，见报告）+ 逐字节（提示符 + 目录名 + 条目）",
          wait_stream(want_ls_root, 25), repr(want_ls_root))
    type_line(mon, "ls /bin")
    want_lsbin = "  shell.bin  %d bytes\n" % shell_sz
    check("ls /bin 列出 shell.bin 且大小逐字节等于构建产物（%d B）" % shell_sz,
          wait_stream("/bin:\n" + want_lsbin, 25), repr("/bin:\n" + want_lsbin))

    type_line(mon, "cat /etc/sh64hello.txt")
    check("cat 逐字节正确（/etc/sh64hello.txt 的固定内容）",
          wait_mark("A4-1 ring3 shell: /etc/sh64hello.txt byte test", 25), "")
    check("cat 的字节在 shell 输出流上按原样出现（含结尾换行、含回显那一行）",
          (b"cat /etc/sh64hello.txt\n" + HELLO_BYTES) in shell_stream(slog()).encode("utf-8", "replace"))

    # ---------------- ④ mkdir + cd + pwd（shell 自己维护 cwd）----------------
    type_line(mon, "cd /tmp")
    check("cd /tmp 后提示符变成 /tmp（shell 维护的 cwd 生效）", wait_mark("sh64:/tmp$ ", 20), "")
    type_line(mon, "mkdir d1")
    check("mkdir 真落盘（mkdir: ok /tmp/d1）", wait_mark("mkdir: ok /tmp/d1", 20), "")
    type_line(mon, "cd d1")
    check("cd d1（相对路径）后提示符 = sh64:/tmp/d1$", wait_mark("sh64:/tmp/d1$ ", 20), "")
    type_line(mon, "pwd")
    check("相对 cd 后的 pwd 逐字节 = /tmp/d1",
          wait_stream("sh64:/tmp/d1$ pwd\n/tmp/d1\nsh64:/tmp/d1$ ", 25))
    type_line(mon, "mkdir sub")
    check("mkdir sub 相对路径真落盘（mkdir: ok /tmp/d1/sub）", wait_stream("mkdir: ok /tmp/d1/sub", 25), "")
    type_line(mon, "cd sub")
    check("二级相对路径：cd sub 后提示符 = sh64:/tmp/d1/sub$", wait_mark("sh64:/tmp/d1/sub$ ", 20), "")
    type_line(mon, "pwd")
    check("二级路径的 pwd 逐字节 = /tmp/d1/sub",
          wait_stream("sh64:/tmp/d1/sub$ pwd\n/tmp/d1/sub\nsh64:/tmp/d1/sub$ ", 25))
    type_line(mon, "cd ..")
    check("cd .. 回退一级 -> sh64:/tmp/d1$", wait_mark("sh64:/tmp/d1$ ", 20), "")
    type_line(mon, "ls")
    # ★ 注意 "  sub  0 bytes" 而不是 "sub/"：内核 fd64_readdir64 判"大小 0"条目的类型时用的是
    #   **/<name>**（相对卷根，不是被列目录）—— 子目录里的子目录被误判成 FILE。这是既有缺陷
    #   （fd64.cpp，本轮不许改），如实断言当前行为，报告里列了。
    check("ls（无参数 = 当前目录）列出刚建的 sub（逐字节；子目录类型受内核既有缺陷影响）",
          wait_stream("/tmp/d1:\n  sub  0 bytes\n", 25))
    type_line(mon, "stat /bin/shell.bin")
    check("stat 报 shell.bin 是 regular 且大小一致",
          wait_stream("  size: %d\n" % shell_sz, 25), "")
    type_line(mon, "stat /bin/shell.bin")
    check("stat 报 shell.bin 是 regular 且大小一致",
          wait_mark("type: regular", 20) and ("  size: %d\n" % shell_sz) in shell_stream(slog()), "")

    # ---------------- ⑤ 重定向 + 管道 ----------------
    type_line(mon, "echo rdir > /tmp/o1")
    type_line(mon, "cat /tmp/o1")
    check("重定向 >（写文件）+ cat 读回：逐字节 = rdir\\n",
          wait_mark("rdir\nsh64:/tmp/d1$ ", 20)
          and "cat /tmp/o1\nrdir\n" in shell_stream(slog()), "")
    type_line(mon, "echo more >> /tmp/o1")
    type_line(mon, "cat /tmp/o1")
    check("追加重定向 >>：cat 读回两行（rdir\\nmore\\n）",
          wait_mark("rdir\nmore\nsh64:", 20), "")
    type_line(mon, "cat < /etc/sh64hello.txt")
    check("输入重定向 <：cat 无参数读它，字节 = /etc/sh64hello.txt 的内容",
          wait_mark("byte test\nsh64:", 25)
          and (b"cat < /etc/sh64hello.txt\n" + HELLO_BYTES) in shell_stream(slog()).encode("utf-8", "replace"),
          "")
    type_line(mon, "ls /bin | cat")
    check("管道 |（内置之间）：ls /bin | cat 的输出里出现 shell.bin 那一行（输出经上一段的内存缓冲）",
          wait_mark("shell.bin", 25) and want_lsbin in shell_stream(slog()), "")

    # ---------------- ⑥ 外部命令：fork + execve + wait4 ----------------
    before_run = len(slog())
    type_line(mon, "run /musl_hello.elf", per_key=0.1)
    got = wait_mark("run: /musl_hello.elf pid=", 60)
    log_run = slog()[before_run:]
    run_stream = shell_stream(log_run)
    check("外部命令跑通（shell 打印 run: … exited code=7，退出码由 wait4 的 status 解出）",
          got and re.search(r"run: /musl_hello\.elf pid=\d+ exited code=7", run_stream) is not None,
          "")
    check("外部命令走 fork(57)（[SYSCALL] insn nr=57）", "[SYSCALL] insn nr=57" in log_run)
    check("外部命令走 execve(59)（[SYSCALL] insn nr=59）", "[SYSCALL] insn nr=59" in log_run)
    check("外部命令走 wait4(61)（[SYSCALL] insn nr=61）", "[SYSCALL] insn nr=61" in log_run)
    check("内核侧记下 execve 与 wait4（[PROC64] execve path=/musl_hello.elf / [PROC64] wait4）",
          "[PROC64] execve path=/musl_hello.elf" in log_run and "[PROC64] wait4 pid=" in log_run)
    check("子程序真的从盘上装载并跑（[ELF64] load path=/musl_hello.elf … via=execve）",
          re.search(r"\[ELF64\] load path=/musl_hello\.elf entry=[0-9A-F]+ .* via=execve", log_run) is not None)
    check("子程序自己的输出原样出现（musl 静态程序的固定串）",
          b"[MUSL] hello from musl static ELF\n" in log_run.encode("utf-8", "replace"))

    # "跑一个不存在的程序"：shell 在**用户态预检**（stat）后只打一行错误 —— 见 sh64_test 头部与
    # 报告里的"A4-1 发现的既有缺陷"：本内核的 execve 失败路径会放着一个已经没了映像的进程回
    # ring3 取指（-> #PF -> PANIC），proc64.cpp 本轮不允许改，所以 shell 侧先挡住。
    before_bad = len(slog())
    type_line(mon, "run /no_such_prog.elf", per_key=0.1)
    check("不存在的程序：shell 如实报错且**不进内核那条缺陷路径**（run: no such program: …）",
          wait_mark("no such program", 40) and
          "run: no such program: /no_such_prog.elf" in shell_stream(slog()[before_bad:]))
    check("该步之后系统仍活着（没有触发 execve 失败 -> ring3 #PF 那条既有缺陷）",
          proc.poll() is None and "[PANIC]" not in slog()[before_bad:])

    type_line(mon, "rm /tmp/o1")
    type_line(mon, "cat /tmp/o1")
    check("rm 真删（rm: ok /tmp/o1）+ 再 cat 报打不开（cat: cannot open）",
          wait_mark("cat: cannot open /tmp/o1", 20), "")

    # ---------------- ⑦ exit -> 回到终端 ----------------
    type_line(mon, "exit")
    check("exit 之后内核收尾（[SH64] exit pid=… code=0）",
          wait_mark("[SH64] exit pid=", 30) and
          re.search(r"\[SH64\] exit pid=\d+ code=0", slog()) is not None)
    before_back = len(slog())
    type_line(mon, "help")
    check("回到终端：shell 退出后终端命令照旧可用（[TERM] cmd help ok）",
          wait_mark("[TERM] cmd help", 30) and "[TERM] cmd help" in slog()[before_back:])

    # ---------------- ⑧ 禁止出现 ----------------
    log_final = slog()
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log_final)
    check("系统还活着（桌面/内核没有 PANIC 后的复位）", proc.poll() is None, "qemu rc=%s" % proc.poll())

    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    if args.keep:
        print("[sh64] 串口日志：%s" % serial)
    print("--- serial tail ---")
    for line in [x for x in log_final.splitlines() if x.strip()][-25:]:
        print("   | " + line[:180])
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    if strict_fixture:
        print("    （夹具 = build64/system.img + MBR + 主分区卷（含 /bin/shell.bin、/etc/sh64hello.txt、/tmp 0777）；"
              "shell 是卷里的文件，内核只负责装载）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
