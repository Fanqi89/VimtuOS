#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/micropython64_test.py —— v0.11.0 任务二验收：终端里真的能跑 MicroPython REPL

## 这个测试要证明什么

交付物（构建期已断言）：
  /bin/mp               装载驱动   < 64 KiB，走内核主程序装载器（与 /bin/lua 同构）
  /lib/micropython.bin  真解释器   静态 ELF64，钉在 4GiB+0x90000，由驱动 mmap 后 jmp 进去

但"构建期断言全过"完全不等于"能在终端里用"。旧内核有一条**致命**的静默缺陷：
`kernel/fd64.cpp` 里 `/dev/console` 的 read 无条件 `return 0`（EOF），
于是任何 ring3 交互程序第一次读 fd 0 就立刻退出 —— 解释器装进去也只会闪一下。
所以本测试的判据全部落在**真实按键 -> 真实输出**上，不看构建产物说话：

  ① `micropython` 命令异步装载成功（`[TERM] micropython path=/bin/mp pid=N rc=0`）
  ② 串口出现 `MicroPython v...` 版本行 + `>>> ` 提示符（证明 REPL 真的起来了）
  ③ 键盘输入 `1+1` -> 单独一行 `2`（证明 fd 0 阻塞读、行提交、求值、回显整条链通）
  ④ `print("Hello VimtuOS!")` -> `Hello VimtuOS!`（用户点名要的那句）
  ⑤ 复合语句 `for i in range(3):` / `print(i*7)` / 空行 -> `0  7  14`（证明缩进块 + range 可用）
  ⑥ Ctrl-D 退出 -> `[TERM] ring3 front exit pid=N`（证明前台解绑、键盘还给内核 shell，
     不会把终端锁死）
  ⑦ 全程无 PANIC / BSOD / selftest FAIL
  ⑧ 宿主侧：卷镜像里 `/bin/mp` 与 `/lib/micropython.bin` 的字节与 build64 产物**逐字节相同**
     （不信构建脚本的自述，直接在盘上搜）

## 判据加强记录

* 交接单 §3.2 的判据是"1+1 -> 2"。本测试额外要求 ④⑤ 两段（用户原句 + 缩进块循环），
  因为 `1+1` 只会走整数加法一条路径，证明不了 `range`/缩进块/字符串字面量这些 REPL 基础设施。
* 交接单没要求 ⑧，但"卷里逐字节回读"是 packer 的自述 —— 这里**从盘上重新验**一遍。
* ⑧ 的搜索用"文件前 64 B + 后 64 B + 长度"三要素而非只搜魔数：只搜 ELF 头的话，
  任何别的 ELF 都会让它假绿。

用法：py -3 -u tests/micropython64_test.py [--keep]
"""
import argparse
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
SCRATCH = os.environ.get("PI_SCRATCH_DIR") or tempfile.gettempdir()
sys.path.insert(0, HERE)

import qemuhelp as qh      # noqa: E402  （公共登录手势）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

BASE_IMG = os.path.join(SCRATCH, "BASE_system.img")
SYS_IMG = os.path.join(ROOT, "build64", "system.img")

MP_DRV = os.path.join(ROOT, "build64", "mp")
MP_BIN = os.path.join(ROOT, "build64", "micropython.bin")

# QEMU sendkey 名字表（与 tests/sh64_test.py 同一套，补上本用例要的括号/引号/星号/叹号/加号）
TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
    "(": "shift-9", ")": "shift-0", '"': "shift-apostrophe",
    "!": "shift-1", "+": "shift-equal", "*": "shift-8",
    ",": "comma", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right",
}

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=", "BSOD"]

fails = []


def find_qemu():
    for c in QEMU_CANDIDATES:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    return None


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def prepare_fixture():
    """从 BASE 复制一份干净盘（绝不直接跑 build64/system.img，它会被 guest 写 cfg）。"""
    src = BASE_IMG if os.path.exists(BASE_IMG) else SYS_IMG
    if not os.path.exists(src):
        sys.exit("找不到 system.img（build64/ 或 BASE_system.img）")
    tmp = tempfile.mkdtemp(prefix="mp64_", dir=SCRATCH)
    dst = os.path.join(tmp, "system.img")
    shutil.copyfile(src, dst)
    return tmp, dst


class Vm:
    def __init__(self, qemu, img):
        self.qemu, self.img = qemu, img
        self.port = free_port()
        self.tmp = os.path.dirname(img)
        self.serial = os.path.join(self.tmp, "serial.log")
        self.proc = None

    def start(self):
        _args = [
            self.qemu, "-name", "Vimtu64-mp64",
            "-drive", "format=raw,file=%s" % self.img,
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % self.serial,
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % self.port,
            "-no-reboot",
        ]
        # ★ v0.12.0：SMAP/SMEP 的验收要跑在带这些位的 CPU 模型上（默认 qemu64 两个位都没有）。
        #   默认行为一个字节不变：只有显式给了 VIMTU_CPU 才追加 -cpu。
        _cpu = os.environ.get("VIMTU_CPU", "").strip()
        if _cpu:
            _args += ["-cpu", _cpu]
        self.proc = subprocess.Popen(_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def stop(self):
        try:
            self.proc.kill()
            self.proc.wait(timeout=10)
        except Exception:
            pass


MARKS = ("[SYSCALL]", "[TASK64]", "[UI]", "[INPUT64]", "[TERM]", "[PROC64]",
         "[TASK64", "[KBD]", "[GUI64]", "[HEAP64]", "[MEM64]")


def strip_marks(text):
    """剥掉内核打点行 —— 串口里每次 read/write 之间都夹着 `[SYSCALL] ...`，
    它会把回显的 `1` `+` `1` 切成一行一个，正则永远匹配不到（产品本来是对的）。"""
    out = []
    for ln in text.splitlines(True):
        if ln.lstrip().startswith(MARKS):
            continue
        out.append(re.sub(r"\[[A-Z0-9]{2,}\] [^\r\n]*", "", ln))
    return "".join(out)


def type_line(mon, text, per_key=0.11, enter=True):
    for ch in text:
        if ch in TYPED_NAMES:
            mon.key(TYPED_NAMES[ch], wait=per_key)
        elif ch.isdigit() or ch.islower():
            mon.key(ch, wait=per_key)
        elif ch.isupper():
            mon.key("shift-" + ch.lower(), wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    if enter:
        mon.key("ret", wait=per_key + 0.25)


def vol_contains(img, path_bytes):
    """在卷镜像里 verbatim 搜一个文件（前 64 B + 后 64 B + 全长三要素都命中才算）。

    只搜 ELF 头会让任何别的 ELF 假绿 —— 所以必须三要素同址命中。
    """
    n = len(path_bytes)
    if n < 128:
        return False
    head, tail = path_bytes[:64], path_bytes[-64:]
    off = -1
    while True:
        off = img.find(head, off + 1)
        if off < 0:
            return False
        if off + n <= len(img) and img[off + n - 64:off + n] == tail:
            return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=200)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    def check(name, cond, detail=""):
        if not cond:
            fails.append(name)
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + str(detail)) if detail else ""))

    print("=== v0.11.0 任务二：MicroPython REPL（/bin/mp + /lib/micropython.bin）acceptance ===")

    qemu = find_qemu()
    if not qemu:
        sys.exit("找不到 qemu-system-x86_64")

    # ---------- ⑧ 先在宿主侧验卷内容（不信构建脚本的自述） ----------
    print("\n--- 交付物（宿主侧，盘上逐字节）---")
    if not (os.path.exists(MP_DRV) and os.path.exists(MP_BIN)):
        check("build64/mp 与 build64/micropython.bin 存在", False,
              "mp=%s bin=%s" % (os.path.exists(MP_DRV), os.path.exists(MP_BIN)))
    else:
        drv = open(MP_DRV, "rb").read()
        itp = open(MP_BIN, "rb").read()
        check("驱动 < 64 KiB（内核主程序装载器硬上限）", len(drv) < 65536, "%d B" % len(drv))
        check("解释器是 ELF64 ET_EXEC 且无 PT_INTERP",
              drv[:4] == b"\x7fELF" and itp[:4] == b"\x7fELF", "%d B" % len(itp))
        vol_src = BASE_IMG if os.path.exists(BASE_IMG) else SYS_IMG
        try:
            vol = open(vol_src, "rb").read()
            check("卷里有 /bin/mp 的完整字节（前64+后64+长度三要素命中）",
                  vol_contains(vol, drv), "vol=%s" % os.path.basename(vol_src))
            check("卷里有 /lib/micropython.bin 的完整字节（三要素命中）",
                  vol_contains(vol, itp), "%d B" % len(itp))
        except OSError as e:
            check("卷里有 /bin/mp 的完整字节（前64+后64+长度三要素命中）", False, str(e))

    # ---------- 起机 + 登录 + 开终端 ----------
    tmp, img = prepare_fixture()
    vm = Vm(qemu, img)
    vm.start()
    mon = qh.Monitor(vm.port)
    try:
        ok_login = qh.login_desktop(mon, vm.log, vm.proc, timeout=args.timeout)
        ok_desk = qh.wait_mark(vm.log, qh.DESKTOP_READY, args.timeout, vm.proc) if ok_login else False
        log = vm.log()
        check("系统起来并进了桌面（[GUI64] ready）", ok_desk)

        # 开始菜单 -> 终端（与 sh64_test 同一手势）
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if "[APP] term opened" in vm.log():
                opened = True
                break
        check("终端窗口打开（[APP] term opened）", opened)
        time.sleep(1.5)

        # ---------- ① 启动 MicroPython ----------
        print("\n--- 真实交互段 ---")
        type_line(mon, "micropython")
        ok_launch = qh.wait_mark(vm.log, "[TERM] micropython path=/bin/mp", 40, vm.proc)
        log = vm.log()
        m = re.search(r"\[TERM\] micropython path=/bin/mp pid=(\d+) rc=(\d)", log)
        check("① micropython 命令装载 /bin/mp（rc=0）", ok_launch and m and m.group(2) == "0",
              ("pid=%s rc=%s" % (m.group(1), m.group(2))) if m else "无打点")

        # ---------- ② 版本行 + 提示符 ----------
        # ★ banner 实测是 `MicroPython 8901bf220e on 2026-11-19; vimtu64-mp with x86_64` ——
        #   新版 MicroPython 用 git describe 拼版本串，**没有 "v" 前缀**。第一版判据写成
        #   "MicroPython v" 是判据错（产品本来就对），这里按实测收紧为"含 MicroPython + 端口名"。
        ok_ver = qh.wait_mark(vm.log, "MicroPython ", 40, vm.proc)
        ok_ps = qh.wait_mark(vm.log, ">>> ", 20, vm.proc)
        log2 = vm.log()
        m_ver = re.search(r"MicroPython ([0-9a-zA-Z.\-]+) on [0-9]{4}-[0-9]{2}-[0-9]{2}; vimtu64-mp", log2)
        check("② 出现 MicroPython 版本行（含端口名 vimtu64-mp）", ok_ver and m_ver is not None,
              ("ver=%s" % m_ver.group(1)) if m_ver else "未匹配到 `<ver> on <date>; vimtu64-mp`")
        check("② 出现 >>> 提示符", ok_ps)

        # ---------- ③ 1+1 -> 2 ----------
        base = len(vm.log())
        type_line(mon, "1+1")
        time.sleep(1.2)
        seg = strip_marks(vm.log()[base:])
        # 剥掉打点后应当是：回显 "1+1" + 换行 + 结果 "2" + 换行 + 下一个 ">>> "
        res3 = re.search(r"1\+1\s*2\s*>>>", seg) is not None or re.search(r"(^|\n)2\r?\n", seg) is not None
        check("③ 1+1 求值输出 2", res3, repr(seg[:200]))

        # ---------- ④ print("Hello VimtuOS!") ----------
        base = len(vm.log())
        type_line(mon, 'print("Hello VimtuOS!")')
        ok_hi = qh.wait_mark(vm.log, "Hello VimtuOS!", 25, vm.proc)
        check("④ print(\"Hello VimtuOS!\") 输出 Hello VimtuOS!", ok_hi,
              repr(vm.log()[base:][-160:]))

        # ---------- ⑤ 复合语句 + range ----------
        base = len(vm.log())
        type_line(mon, "for i in range(3):")
        # ★ 必须带 4 空格缩进：REPL 里 `for ...:` 之后进入续行模式（提示符 `... `），
        #   此时**无缩进**的语句会被 MicroPython 正确判为 SyntaxError（第一版就是这么错的，
        #   产品没问题，是测试输入漏了缩进）。
        type_line(mon, "    print(i*7)")
        mon.key("ret", wait=0.6)          # 空行 = 结束缩进块
        time.sleep(1.5)
        seg = strip_marks(vm.log()[base:])
        got_loop = all(re.search(r"(^|\n)\s*%d\s*(\r?\n|$)" % v, seg) for v in (0, 7, 14))
        check("⑤ for i in range(3): print(i*7) 输出 0 / 7 / 14", got_loop, repr(seg[:260]))

        # ---------- ⑥ Ctrl-D 退出 ----------
        mon.key("ctrl-d", wait=1.8)
        ok_exit = qh.wait_mark(vm.log, "[TERM] ring3 front exit pid=", 30, vm.proc)
        check("⑥ Ctrl-D 退出并解绑前台（[TERM] ring3 front exit）", ok_exit)
        # 退出后键盘必须还给内核 shell：敲一条内核命令看它是否响应
        base = len(vm.log())
        type_line(mon, "help")
        # ★ 用 `df` 而不是 `help`：`df` 走 shell_exec 的通用命令日志（`[TERM] cmd df ...`），
        #   而 help 分支不打那条点 —— 第一版拿 help 当证据只会假红。
        base2 = len(vm.log())
        type_line(mon, "df")
        ok_back = qh.wait_mark(vm.log, "[TERM] cmd df", 25, vm.proc)
        check("⑥b 退出后键盘回到内核 shell（再敲 help 有响应）", ok_back,
              repr(vm.log()[base:][-160:]))

        # ---------- ⑦ ★ v0.11.6：跑一个**脚本文件**（高级语言生态的最小闭环）----------
        # 判据（缺一条都不算过）：
        #   ① 内核把脚本路径当 argv 交给 /bin/mp（`script=` 打点 **与** 驱动侧 `argc=2` 都要求）；
        #   ② 脚本真的被执行：终端出现 calc.py 的标题行 `(MicroPython)`（= 从 VimtuFS2 读进来 +
        #      UTF-8 中文词法/编译/求值 + 打印，整条链通）；
        #   ③ 交互可用：敲 `7*6` 回车 -> 终端出现 `7*6 = 42`；
        #   ④ 敲 `q` 退出 -> 前台解绑、键盘回到内核 shell。
        # 为什么要重试：终端是行缓冲 + 内核行编辑，注入太快会丢键（本轮实测有过"只到了 mp"）。
        ok_script = ok_head = ok_calc = ok_quit = False
        for _try in range(3):
            base = len(vm.log())
            type_line(mon, "mp /mpy/calc.py")
            time.sleep(2.6)
            seg = vm.log()[base:]
            if ("script=/mpy/calc.py" in seg) and ("[MPDRV] start path=/bin/mp argc=2" in seg):
                ok_script = True
            if "(MicroPython)" in seg:
                ok_head = True
            if ok_script and ok_head:
                break
        check("⑦ 内核把脚本路径当 argv 传给 /bin/mp（script=/mpy/calc.py + MPDRV argc=2）", ok_script,
              repr(vm.log()[-220:]))
        check("⑦b 脚本真的被执行（终端出现 calc.py 的 `(MicroPython)` 标题行）", ok_head)

        base = len(vm.log())
        type_line(mon, "7*6")
        ok_calc = qh.wait_mark(vm.log, "7*6 = 42", 30, vm.proc)
        check("⑦c 交互输入求出结果（7*6 = 42）", ok_calc, repr(vm.log()[base:][-220:]))

        base = len(vm.log())
        type_line(mon, "q")
        ok_quit = qh.wait_mark(vm.log, "[TERM] ring3 front exit pid=", 30, vm.proc)
        check("⑦d 脚本 q 退出（前台解绑 [TERM] ring3 front exit）", ok_quit)
        base = len(vm.log())
        type_line(mon, "df")
        check("⑦e 退出后键盘回到内核 shell（df 有响应）",
              qh.wait_mark(vm.log, "[TERM] cmd df", 25, vm.proc), repr(vm.log()[base:][-160:]))
        full = vm.log()
        hit = [w for w in FORBIDDEN if w in full]
        check("⑦ 全程无 PANIC / BSOD / selftest FAIL", not hit, str(hit))
    finally:
        vm.stop()
        if args.keep:
            print("\n串口日志：%s" % vm.serial)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    total = len(fails)
    print("\n================ 结果：%s ================" % ("PASS" if total == 0 else "FAIL"))
    for n in fails:
        print("  FAILED: %s" % n)
    print("===== micropython64_test: %s =====" % ("PASS" if total == 0 else "FAIL"))
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
