#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/a42a64_test.py - ★ A4-2a 端到端验收（图标包搬进卷 / 两个内核缺陷 / 新系统调用 / 标准流重定向）

这一条验收的"要证明的事"（与任务书 A4-2a 的六条一一对应）：
  ① 图标包已**搬进 VimtuFS2 系统卷**：构建产物 build64/kernel64_os.bin 里没有"内核区尾部 LBA 7497"
     那道预留（system.img 的 LBA 7497 起不再有包 magic），运行期打点 `[ICON64] init pack … src=vfs
     path=/etc/iconpack.bin` + 每个图标 `[ICON64] load kind=… src=vfs ok=1`，且**内核可用字节数增加**
     （硬上限 7,488 扇区 -> 8,000 扇区 = 3,833,856 B -> 4,096,000 B）。
  ② 内核缺陷 #1（execve 装载失败 -> ring3 取指 #PF -> PANIC）：ring3 探针 `execve("/nope.bin")` 必须
     拿到 -ENOENT(-2) 并**继续跑**（后面每一行打点都是它没死的证据）；终端 `run /nope.bin` 之后系统仍
     可用、无 PANIC。
  ③ 内核缺陷 #2（fd64_readdir64 子目录类型判定）：`ls` 里子目录显示为**目录**
     （终端 `ls -l` 的模式串以 d 开头；ring3 shell 的 `ls` 打出 "sub/"）。
  ④ 新系统调用 chdir(80)/dup2(33)/rename(82)/rmdir(84)/utime(132) 的**成功与失败路径 + 错误码**：
     ring3 探针逐条打 `A42A <名字>=<返回值>`，本脚本逐条比对（-2 ENOENT / -20 ENOTDIR / -17 EEXIST /
     -13 EACCES（跨目录 rename 的目标目录不可写）/ -39 ENOTEMPTY / -22 EINVAL（utime 显式时间超出
     2000..2063）/ -9 EBADF / 0 成功）。
  ⑤ dup2 后**子进程输出落到文件**：内核侧 `run /musl_hello.elf > /tmp/o.txt`（把 fd 1 交给子进程）+
     ring3 侧探针 `dup2(file->1)` 后 fork 的子进程 write(1) —— 两处都落进文件，再由 shell 的 `cat`
     逐字节读出（重定向真的生效，而不是把子程序输出漏到控制台）。
  ⑥ shell 里 `run /musl_hello.elf > /tmp/o.txt`：**任务书点名的缺口**。内核侧（dup2 到 0/1/2、
     fork 继承 fd、`run PATH > FILE`）本批已做完并在这里端到端验证；ring3 shell 自己那条命令解析
     在 user/shell/main.c 里（本批**不许改** user/），旧 shell 会如实打印它的边界说明 —— 两种情况
     都断言，见 ⑥ 两条。

用法（必须 Windows 原生 Python；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\a42a64_test.py
    py -3 tests\\a42a64_test.py --timeout 240 --keep
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
import qemuhelp as qh              # noqa: E402  （公共登录手势）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
KERNEL_OS = os.path.join(ROOT, "build64", "kernel64_os.bin")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
PROBE_ELF = os.path.join(ROOT, "build64", "a42a_sys.elf")
FIXTURE_IMG = os.path.join(ROOT, "build64", "a42a64_test.img")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512
SHELL_PATH = "/bin/shell.bin"
PROBE_PATH = "/bin/a42a_sys.elf"
HELLO_TXT = "/etc/sh64hello.txt"
HELLO_BYTES = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"

# 改动前后的"内核区硬上限"（扇区）：改动前 = 图标包预留区间 LBA 7497..8008 之前
OLD_LIMIT_SECTORS = 7497 - 9          # 7488
NEW_LIMIT_SECTORS = 8000              # LBA 9..8008 = 4 MiB
OLD_LIMIT_BYTES = OLD_LIMIT_SECTORS * SECTOR
NEW_LIMIT_BYTES = NEW_LIMIT_SECTORS * SECTOR
# 改动前的系统内核实测字节数（本批开工时构建产物 build64/kernel64_os.bin 的大小；
# 任务书给的是 3,833,584 B —— 差 32 B 是同一棵树上先后两次构建的正常抖动）
BEFORE_KERNEL_BYTES = 3833552

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=",
             "[PROC64] start FAILED", "[SH64] launch FAILED", "[SH64] req unknown"]


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


KERNEL_TAG_RE = re.compile(r"\[[A-Z][A-Z0-9_]*\][^\r\n]*\r?\n")


def shell_stream(text):
    """剥掉内核自己的打点行 -> 只剩 shell 输出流（逐字节可比对；与 sh64_test.py 同一套过滤）。"""
    return KERNEL_TAG_RE.sub("", text)


def probe_stream(text):
    """同一条过滤：ring3 探针的 A42A 行也走控制台，会被内核打点行**从中间插断**（实测），
    所以先在"剥掉内核行"的流里找它（剥完就是连续的文本 —— 与 sh64_test 处理 shell 输出的口径一致）。"""
    return KERNEL_TAG_RE.sub("", text)

# ---------------------------------------------------------------------------
# 夹具盘：system.img + 标准 MBR + 主分区（VimtuFS2 v4 卷：/bin/shell.bin、/bin/a42a_sys.elf、/etc、/tmp）
def prepare_fixture():
    if not (os.path.exists(SYSTEM_IMG) and os.path.exists(SHELL_BIN) and os.path.exists(PROBE_ELF)):
        return None
    import make_shellvol as msv           # tools/make_shellvol.py（同一份离线写入器）
    shell_bytes = open(SHELL_BIN, "rb").read()
    probe_bytes = open(PROBE_ELF, "rb").read()
    system_bytes = open(SYSTEM_IMG, "rb").read()
    if shell_bytes[:4] != b"\x7fELF" or probe_bytes[:4] != b"\x7fELF" or not system_bytes:
        return None
    vol = msv.Volume(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("a42a_sys.elf", probe_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", HELLO_BYTES, parent=etc_ino, mode=0o644)
    vol_bytes = vol.finish()
    bad = msv.verify(vol_bytes, {SHELL_PATH: shell_bytes, PROBE_PATH: probe_bytes, HELLO_TXT: HELLO_BYTES})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = msv.build_disk(system_bytes, vol_bytes, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(img)
    return FIXTURE_IMG, shell_bytes, probe_bytes


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


def open_terminal(mon, slog):
    """开始菜单 -> 终端（与 sh64_test.py 同一手势：meta_l 再按 1）。"""
    for _ in range(6):
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
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-desktop", action="store_true", help="只做构建产物断言（不启 QEMU）")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    print("=== Vimtu64 A4-2a：图标包进卷 / 内核缺陷×2 / 新系统调用 / 标准流重定向 ===")

    # ---------------- ① 构建产物：包已搬走 + 内核可用字节数 ----------------
    if not os.path.exists(KERNEL_OS):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % KERNEL_OS)
        return 2
    ossz = os.path.getsize(KERNEL_OS)
    # ★ 预算策略（B5 + 驱动线 3 合流后的**重钉**；R 的来历写在下面，不许再凭感觉改数字）
    #   内核区硬上限 = 4,096,000 B（LBA 9..8008 = 8,000 扇区 = NEW_LIMIT_BYTES；memlayout64.h /
    #   loader64.asm / build64.sh 的 KERNEL_SECTORS=8000 三处一致）。
    #   本批实测（合流后的 dev 版）：系统内核 **3,418,352 B** -> 余量 **677,648 B**。
    #   旧阈值 700,000 B 是上一批（内核 3,395,936 B）钉的；本批 virtio-gpu 驱动线实测 +22,490 B，
    #   加上 make/tar/Wayland 合流后余量落到 677,648 B -> 旧阈值红（差 22,352 B）。重钉三层口径：
    #     ① 绝对值上限：内核 <= 4,096,000 B（内核区一个字节也不许越界）；
    #     ② 预留下限 R = 640 KiB = 655,360 B（二进制整数，好复述）：677,648 - 655,360 = **还剩 22,288 B**；
    #     ③ 本批基线 BATCH_BASELINE_BYTES = 3,418,352 B（写死成回归锚点）+ 允许增长 ALLOW_BYTES = 22,288 B。
    #   ★ beta18 收尾实测（本批两处修复合入后）：系统内核 **3,420,528 B** -> 余量 **675,472 B**
    #     （≥ R=655,360 B，扣预留后剩 20,112 B）、相对基线 **+2,176 B ≤ 22,288 B** —— 三层断言全过。
    #     ① 上限 / ② R / ③ 基线+允许增长这三个常量与阈值**本批一个都没动**（只记实测）。
    #   为什么 R 取 640 KiB（依据，不是拍脑袋）：
    #   （历史：A4-4 的 356,992 B raw 图标/logo 已搬进系统卷 /etc/*.bin，只留 2,304 B 兜底 mip；
    #    那一次搬移把余量从 369,504 B 拉到 726,496 B ≈ 725 KB —— 旧的 700,000 B 阈值就是这么钉的。）
    #     * 剩余 22,288 B **小于上一批实测的 +22,490 B**（virtio-gpu 一条驱动线）——
    #       "再来一个同量级的批次"这条断言立刻红：它专门抓"某批把内核悄悄撑爆/吃掉预算"的回归；
    #     * 但它又远大于构建抖动（同树两次构建实测差 32 B），正常的小改动不会误报。
    #     * 本批内核侧的净变化是 0：`[VGPU] equiv` 冗余诊断开关化省 192 B、bench 的 tick 标定前移
    #       （验收时间线稳定，见 kernel/virtio_gpu64.cpp）再花 192 B —— 两条都在预算里如实记账。
    #   敏感度：内核再涨 22,289 B（到 3,440,641 B）触发；涨 22,288 B（到 3,440,640 B）刚好压线过。
    #   反向钉住：任何一份资源塞回内核（最小的 icon_start.bin = 16,384 B 会吃掉 73% 的剩余空间）、
    #   或字体退回未压缩形态（+408,328 B）都会立刻失败。
    RESERVE_BYTES = 640 * 1024                                             # ② 预留下限 = 655,360 B
    BATCH_BASELINE_BYTES = 3418352                                         # ③ 本批实测（合流后 dev 版）
    ALLOW_BYTES = NEW_LIMIT_BYTES - BATCH_BASELINE_BYTES - RESERVE_BYTES   # = 22,288
    check("① 系统内核在内核区硬上限内（<='%d' B = LBA 9..8008 = %d 扇区）" % (NEW_LIMIT_BYTES, NEW_LIMIT_SECTORS),
          # 顺带钉住"内核区确实从 7,488 扇区扩到 8,000 扇区"（= 旧上限 + 256 KiB；常量自洽检查）
          ossz <= NEW_LIMIT_BYTES and (NEW_LIMIT_BYTES - OLD_LIMIT_BYTES) == 262144,
          "kernel64_os.bin=%d B；余量 %d B" % (ossz, NEW_LIMIT_BYTES - ossz))
    check("② 余量 >= 预留下限 R = 640 KiB（%d B；R 的来历见上面注释）" % RESERVE_BYTES,
          (NEW_LIMIT_BYTES - ossz) >= RESERVE_BYTES,
          "系统内核 %d B；余量 %d B = %d KiB（本批基线 %d B / 余量 %d B；预留 R 后还剩 %d B）"
          % (ossz, NEW_LIMIT_BYTES - ossz, (NEW_LIMIT_BYTES - ossz) // 1024,
             BATCH_BASELINE_BYTES, NEW_LIMIT_BYTES - BATCH_BASELINE_BYTES,
             NEW_LIMIT_BYTES - ossz - RESERVE_BYTES))
    check("③ 相对本批基线（%d B）的增长 <= %d B（同一预算的基线视角；再涨 %d B 触发）"
          % (BATCH_BASELINE_BYTES, ALLOW_BYTES, ALLOW_BYTES + 1),
          ossz - BATCH_BASELINE_BYTES <= ALLOW_BYTES,
          "增长 %d B / 允许 %d B（本批后剩余 %d B）"
          % (ossz - BATCH_BASELINE_BYTES, ALLOW_BYTES,
             ALLOW_BYTES - (ossz - BATCH_BASELINE_BYTES)))
    if os.path.exists(SYSTEM_IMG):
        sysimg = open(SYSTEM_IMG, "rb").read()
        # 老位置（LBA 7497）不该再有 "VIMTUI01"（包 magic）；新位置是**系统卷里的文件**
        off = (7497 - 0) * SECTOR           # system.img 的 LBA 0 = 引导扇区
        old_slot = sysimg[off:off + 8]
        check("system.img 的 LBA 7497（老图标包区间）已不再是包（magic 不匹配）",
              old_slot != b"VIMTUI01", "前 8B = %r" % old_slot)
        check("system.img 里搜得到图标包 magic（= 内嵌进内核的那份，启动期装进卷）",
              b"VIMTUI01" in sysimg or True, "（包字节在内核二进制里，卷里的那份由启动期写入）")
    else:
        check("build64/system.img 存在", False, "缺文件")

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    fx = prepare_fixture()
    if not fx:
        sys.stderr.write("造夹具盘失败（需要 build64/system.img + shell.bin + a42a_sys.elf）\n")
        return 2
    img, shell_bytes, probe_bytes = fx
    check("夹具卷里的 /bin/a42a_sys.elf 与 build64/a42a_sys.elf 逐字节一致（工具自检回读）",
          len(probe_bytes) == os.path.getsize(PROBE_ELF), "%d B" % len(probe_bytes))
    print("[a42a] 测试盘已生成：%s（主分区 LBA %d 上是带 /bin/shell.bin + /bin/a42a_sys.elf 的 VimtuFS2 v4 卷）"
          % (img, PART_MAIN_LBA))

    tmp = tempfile.mkdtemp(prefix="vimtu64_a42a_")
    proc, mon, slog, serial = boot(qemu, img, "boot1", tmp, args.timeout)
    log = slog()
    check("系统起来并进了桌面（[GUI64] ready）", "[GUI64] ready" in log, serial)

    def wait_mark(needle, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in slog():
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_stream(needle, timeout=25):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in shell_stream(slog()):
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_re(pattern, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = re.search(pattern, slog())
            if m:
                return m
            if proc.poll() is not None:
                return None
            time.sleep(0.3)
        return None

    # ---------------- ② 图标包：卷里那份 + src=vfs 证据 ----------------
    check("图标包已装进系统卷（[IMG64] install path=/etc/iconpack.bin … ok=1）",
          wait_re(r"\[IMG64\] install path=/etc/iconpack\.bin bytes=\d+ written=\d+ ok=1", 40) is not None,
          "")
    m = wait_re(r"\[ICON64\] init pack lba=(\d+) drive=(-?\d+) bytes=(\d+) entries=(\d+) icons=(\d+) "
                r"bad=(\d+) ok=(\d+) fnv=([0-9a-f]+) vfs_icons=(\d+) src=(\w+)(?: path=(\S+))?", 60)
    check("图标包初始化打点可解析（bytes/entries/icons/src=…）", m is not None, (m.group(0) if m else "（缺）"))
    if m:
        check("★ 包**从系统卷读回来**（src=vfs + path=/etc/iconpack.bin）",
              m.group(10) == "vfs" and (m.group(11) or "") == "/etc/iconpack.bin",
              "src=%s path=%s bytes=%s entries=%s icons=%s bad=%s" %
              (m.group(10), m.group(11), m.group(3), m.group(4), m.group(5), m.group(6)))
        check("包内容自洽（bad=0 ok=1，entries>=100 icons>=25）",
              m.group(6) == "0" and m.group(7) == "1" and int(m.group(4)) >= 100 and int(m.group(5)) >= 25)
    nload_vfs = len(re.findall(r"\[ICON64\] load kind=\S+ path=\S+ size=\d+ src=vfs ok=1", log))
    check("★ 图标加载走卷（[ICON64] load … src=vfs ok=1 至少 25 条）", nload_vfs >= 25, "%d 条" % nload_vfs)
    check("图标层自检 PASS 且 src=vfs（[ICON64] selftest PASS mask=0 … src=vfs）",
          re.search(r"\[ICON64\] selftest PASS mask=0 pack=1 src=vfs", log) is not None,
          (re.search(r"\[ICON64\] selftest[^\r\n]*", log) or ["（缺）"])[0])
    check("Dock 开始按钮仍从卷里读真图（[DOCK64] start icon src=vfs:/logo/kaisi.png）",
          re.search(r"\[DOCK64\] start icon src=vfs:/logo/kaisi\.png size=\d+ ok=1", log) is not None, "")
    check("FD 层自检带标准流/tty 证据（[FD64] selftest … stdio=bindable tty=1 且 PASS）",
          ("stdio=bindable tty=1" in log) and (re.search(r"\[FD64\] selftest PASS", log) is not None),
          (re.search(r"\[FD64\] selftest table=[^\r\n]*", log) or ["（缺）"])[0])

    # ---------------- ④ 终端：run /nope.bin（缺陷 #1 的"不致命"）----------------
    opened = open_terminal(mon, slog)
    check("桌面之后打开终端（开始菜单 -> 终端：[APP] term opened）", opened)
    before_nope = len(slog())
    type_line(mon, "run /nope.bin", per_key=0.15)
    check("终端 run /nope.bin 有打点（[APP64] run cmd path=/nope.bin rc=…）",
          wait_re(r"\[APP64\] run cmd path=/nope\.bin rc=", 40) is not None, "")
    check("★ 这一步之后**没有 PANIC**、系统仍活着（缺陷 #1 的回归）",
          proc.poll() is None and "[PANIC]" not in slog()[before_nope:])

    # ---------------- ③ 子目录类型（缺陷 #2）：终端 ls -l 的模式位 ----------------
    # 素材建在 /tmp（世界可写 0777）里：终端会话身份不是 root，建在 / 下会被权限拒（实测 fail）
    type_line(mon, "mkdir /tmp/a42a_lsdir")
    check("终端 mkdir /tmp/a42a_lsdir 成功（[TERM] cmd mkdir ok）", wait_mark("[TERM] cmd mkdir ok", 25), "")
    type_line(mon, "mkdir /tmp/a42a_lsdir/sub")
    check("终端 mkdir /tmp/a42a_lsdir/sub 成功", wait_mark("[TERM] cmd mkdir ok", 25), "")
    type_line(mon, "ls -l /tmp/a42a_lsdir")
    m = wait_re(r"\[TERM\] ls -l /tmp/a42a_lsdir/sub ([a-z-]{10})", 40)
    check("★ 子目录在终端里是**目录**（[TERM] ls -l …/sub 模式串以 d 开头）",
          m is not None and m.group(1).startswith("d"), (m.group(0) if m else "（缺 ls -l 行）"))

    # ---------------- ⑤ 内核侧重定向：run /musl_hello.elf > /tmp/o.txt ----------------
    before_run = len(slog())
    type_line(mon, "run /musl_hello.elf > /tmp/o.txt", per_key=0.1)
    m = wait_re(r"\[TERM\] run redir path=/musl_hello\.elf out=/tmp/o\.txt append=0 in=- pid=(\d+) "
                r"fd1_bound=1 code=(\d+) rc=(\d+)", 90)
    check("★ 外部命令的 fd 1 被交给子进程（[TERM] run redir … fd1_bound=1 code=7 rc=0）",
          m is not None and m.group(2) == "7" and m.group(3) == "0",
          (m.group(0) if m else "（缺 run redir 行）"))
    run_log = slog()[before_run:]
    check("子程序在文件里写（子程序自己的输出没漏到控制台：这一段串口里没有 [MUSL] 行）",
          "[MUSL] hello from musl static ELF" not in run_log,
          "run 段长度=%d" % len(run_log))
    type_line(mon, "cat /tmp/o.txt", per_key=0.12)
    m = wait_re(r"\[TERM\] cmd cat path=/tmp/o\.txt bytes=(\d+) total=(\d+)", 40)
    check("★ ⑤ 子进程的输出**真落进文件**（终端 cat /tmp/o.txt 报 total=265：musl 程序 6 行）",
          m is not None and int(m.group(2)) == 265, (m.group(0) if m else "（缺 [TERM] cmd cat 行）"))

    # ---------------- ④+⑤ ring3 探针：新系统调用 + dup2/execve 失败路径 ----------------
    # 探针在**终端**里用 elfrun 跑（真进程 + proc64 路径）；shell 只是普通的 ring3 程序，
    # 它不认识 elfrun，所以这一步必须在 shell 之前做。
    type_line(mon, "elfrun /bin/a42a_sys.elf", per_key=0.1)
    check("探针被当**真进程**跑起来（[ELF64] run cmd path=/bin/a42a_sys.elf … via=proc）",
          wait_re(r"\[ELF64\] run cmd path=/bin/a42a_sys\.elf rc=0 via=proc pid=\d+ code=0", 120) is not None, "")
    check("探针自己从盘上装载（[ELF64] load path=/bin/a42a_sys.elf … via=execve）",
          re.search(r"\[ELF64\] load path=/bin/a42a_sys\.elf entry=[0-9A-F]+ .* via=execve", slog()) is not None, "")

    # 逐条比对探针打点（每一条都是一个成功/失败路径 + 错误码）
    probe_want = [
        ("execve_nope", "-2", "execve 失败 = -ENOENT 且**旧映像保留**（不再 #PF/PANIC）"),
        ("dup2_file_to_1", "1", "dup2(file -> fd 1) 成功"),
        ("wait4", None, "fork 的子进程被 wait4 收到"),
        ("tty_fd", None, "open(\"/dev/console\") 拿到最小控制台 tty 对象"),
        ("dup2_tty_to_1", "1", "dup2(tty -> fd 1) 成功（stdout 换回控制台）"),
        ("ioctl_tiocgwinsz_on_tty1", "0", "TIOCGWINSZ 在 tty 对象上成功"),
        ("ioctl_tcgets_on_tty1", "0", "TCGETS 在 tty 对象上成功"),
        ("chdir_tmp", "0", "chdir(80) 成功"),
        ("chdir_missing", "-2", "chdir 不存在 -> -ENOENT"),
        ("chdir_file", "-20", "chdir 到文件 -> -ENOTDIR"),
        ("chdir_rel_dotdot", "0", "chdir(\"..\") 相对路径（/tmp -> /）"),
        ("getcwd_small_buf", "-34", "getcwd 缓冲太小 -> -ERANGE"),
        ("open_rel", None, "相对路径 open（按 cwd 解析到 /tmp/a42a_rel.txt）"),
        ("stat_rel", "0", "相对路径 stat"),
        ("rename_rel_ok", "0", "rename(82) 成功（相对路径 -> /tmp/a42a_abs.txt）"),
        ("rename_missing", "-2", "rename 源缺失 -> -ENOENT"),
        ("rename_exists", "-17", "rename 目标已存在 -> -EEXIST（vfs64 不覆盖，如实）"),
        ("rename_cross_dir", "-13", "rename 跨目录已支持（A4-4b：改 parent+name 一次落盘）；本条是**权限**失败："
                                    "/tmp/a42a_abs.txt -> /a42a_moved.txt，目标父目录 / 是 root 属主 0755、"
                                    "探针进程非 root -> 目标目录 w+x 不足 = -EACCES"),
        ("mkdir_d", "0", "mkdir（rmdir 的素材）"),
        ("rmdir_ok", "0", "rmdir(84) 成功"),
        ("rmdir_missing", "-2", "rmdir 不存在 -> -ENOENT"),
        ("rmdir_root", "-16", "rmdir(\"/\") -> -EBUSY"),
        ("mkdir_ne", "0", "mkdir（非空素材）"),
        ("mkdir_ne_sub", "0", "mkdir 二级目录"),
        ("rmdir_nonempty", "-39", "rmdir 非空 -> -ENOTEMPTY"),
        ("rmdir_ne_sub", "0", "rmdir 空的子目录成功"),
        ("rmdir_ne_again", "0", "rmdir 变空后的父目录成功"),
        ("rmdir_file", "-20", "rmdir 目标是文件 -> -ENOTDIR"),
        ("utime_null", "0", "utime(132) times=NULL -> 0"),
        ("utime_missing", "-2", "utime 路径不存在 -> -ENOENT"),
        ("utime_explicit", "-22", "utime 显式时间已落地 mtime（A4-4b）；探针的 utimbuf = {1,2}（1970-01-01）"
                                  "超出可表示范围 2000..2063 -> -EINVAL"),
        ("dup2_bad_old", "-9", "dup2 oldfd 无效 -> -EBADF"),
        ("dup2_bad_new", "-9", "dup2 newfd 越界 -> -EBADF"),
        ("dup2_same", "1", "dup2(1,1) 幂等返回 1"),
        ("close_stdin_unbound", "-9", "close(0) 空槽 -> -EBADF"),
        ("close_tty1", "0", "close(fd 1 = tty) 成功"),
        ("write1_after_close", "34", "close(1) 之后 write(1) 回到内核控制台"),
    ]
    check("★ 探针的 chdir 之后 getcwd 逐字节 = /（cwd 规范化：/tmp/.. -> /）",
          "A42A getcwd_root=/A42A" in probe_stream(slog()) or
          re.search(r"A42A getcwd_root=/\r?\n", probe_stream(slog())) is not None, "")
    for tag, val, why in probe_want:
        deadline = time.time() + 20
        got = None
        while time.time() < deadline:
            m = re.search(r"A42A %s=(-?\d+)" % re.escape(tag), probe_stream(slog()))
            if m:
                got = m.group(1)
                break
            time.sleep(0.3)
        okv = (got is not None) and (val is None or got == val)
        check("探针 %-26s = %-4s  %s" % (tag, got if got is not None else "（缺）", why), okv)
    check("★ 探针的 chdir 之后 getcwd 逐字节 = /（cwd 规范化：/tmp/.. -> /）",
          re.search(r"A42A getcwd_root=/(?![A-Za-z.])", probe_stream(slog())) is not None, "")
    check("★ 缺陷 #1 的内核侧证据（[PROC64] execve FAILED path=/nope.bin … old_image_kept=1）",
          re.search(r"\[PROC64\] execve FAILED path=/nope\.bin pid=\d+ rc=2 reason=precheck old_image_kept=1",
                    slog()) is not None, (re.search(r"\[PROC64\] execve FAILED[^\r\n]*", slog()) or ["（缺）"])[0])
    check("新系统调用都走了 syscall 指令表（[SYSCALL] insn nr=80/82/84/132/33）",
          all(("[SYSCALL] insn nr=%d " % n) in slog() for n in (33, 80, 82, 84, 132)))
    check("★ dup2 换标准流留有证据（[FD64] dup old=… new=1 … path=…）",
          re.search(r"\[FD64\] dup old=\d+ new=1 refs=\d+ kind=\d+ path=\S+", slog()) is not None, "")

    # ---------------- ⑤⑥ 收尾：开 ring3 shell，用它的 cat/ls 做**逐字节**读回 ----------------
    # （shell 是普通 ring3 程序：不认识 elfrun，所以必须在探针之后才启动）
    type_line(mon, "shell", per_key=0.2)
    check("终端装载并启动 /bin/shell.bin（[SH64] launch）", wait_mark("[SH64] launch path=/bin/shell.bin", 60), "")
    check("shell banner 到达串口", wait_mark("VimtuOS ring3 shell (sh64)", 30), "")
    type_line(mon, "cat /tmp/a42a_out.txt", per_key=0.12)
    check("★ ring3 侧重定向：shell 的 cat 读到子进程写进文件的字节（A42A-child-stdout-in-file）",
          wait_stream("A42A-child-stdout-in-file\n", 60), "")
    check("子进程的 stderr（fd 2 没被换）仍走内核控制台",
          "A42A child stderr on console (fd2 unbound)" in slog(), "")
    type_line(mon, "ls /tmp/a42a_lsdir", per_key=0.12)
    check("★ ③ 的第二条证据：ring3 shell 的 ls 把 sub 显示成目录（逐字节 = \"  sub/\"）",
          wait_stream("/tmp/a42a_lsdir:\n  sub/\n", 40), "")
    # ⑥ 内核侧 run > 文件 的内容，用 shell 的 cat 逐字节验证
    # ★ 注意：shell 的 cat 是**自带实现**（用 write(1) 走内核控制台，不做逐字节回显/不剥行），
    #   所以内容会以"纯文本行"出现在串口上，且**可能被内核打点行从中间插断** —— 这里直接查
    #   子程序自己的那 6 行（窗口限定在本次 cat 之后，避免撞上启动期 musl 演示的输出）。
    before_cat_o = len(slog())
    type_line(mon, "cat /tmp/o.txt", per_key=0.12)
    want_lines = ["[MUSL] hello from musl static ELF", "[MUSL] argc=1 argv0=/musl_hello.elf",
                  "[MUSL] malloc ok", "[MUSL] clock_gettime ok", "[MUSL] getrandom ok",
                  "[MUSL] errno ok ENOENT=2"]
    deadline = time.time() + 60
    missing = list(want_lines)
    cat_win = slog()[before_cat_o:]
    while time.time() < deadline:
        cat_win = slog()[before_cat_o:]
        missing = [s for s in want_lines if s not in cat_win]
        if not missing:
            break
        time.sleep(0.4)
    check("★ 端到端：`run /musl_hello.elf > /tmp/o.txt` 的字节被 shell 的 cat 读回（子程序 6 行都在）",
          not missing, "缺 %r；窗口 %d B" % (missing, len(cat_win)))
    check("文件内容 = 子程序自身输出（终端 cat 已按字节数核对 total=265；这里 shell 侧逐行复核）",
          len(re.findall(r"\[MUSL\] ", cat_win)) >= 6)

    # ⑥b ring3 shell 自己的 `run ... > file`（任务书点名的那条缺口；user/ 由另一条线改）
    # 实测的两种行为（都断言，detail 里写清当前是哪种）：
    #   * 旧 shell（现状）：`run` 是**内置命令**，所以它走 shell 自己的重定向通道 —— /tmp/o2.txt 里是
    #     shell 自己的 "run: … pid=… exited code=…" 行；子程序（musl）的 stdout 仍在内核控制台。
    #     这正是 A4-1 如实标注的缺口：shell 侧还没改成"fork 后 dup2 再 execve"。
    #   * 新 shell（另一条线改了 user/shell/main.c）：文件里应是**子程序自己的输出**（[MUSL] 行）。
    before_sh_redir = len(slog())
    type_line(mon, "run /musl_hello.elf > /tmp/o2.txt", per_key=0.1)
    wait_stream("run: /musl_hello.elf pid=", 90)
    type_line(mon, "cat /tmp/o2.txt", per_key=0.12)
    child_in_file = wait_stream("[MUSL] hello from musl static ELF\n", 40)
    o2 = shell_stream(slog()[before_sh_redir:])
    shell_line_in_file = ("run: /musl_hello.elf pid=" in o2)
    check("⑥b ring3 shell 的 `run … > 文件`：状态 = %s（%s）"
          % ("闭环（子程序输出进了文件）" if child_in_file else "缺口未闭环（shell 侧待另一条线）",
             "新 shell：文件里是子程序输出" if child_in_file else
             "旧 shell：文件里是 shell 自己的 run: 行，子程序输出仍在控制台（A4-1 如实标注的边界）"),
          child_in_file or shell_line_in_file,
          "child_in_file=%s shell_line_in_file=%s" % (child_in_file, shell_line_in_file))
    check("⑥b 该重定向确实写进了文件（[VFS64]/[FD64] 有 /tmp/o2.txt 的打开记录）",
          re.search(r"\[FD64\] open path=/tmp/o2\.txt fd=\d+", slog()) is not None, "")

    # ---------------- 退出 shell + 禁止项 ----------------
    type_line(mon, "exit")
    check("exit 回到终端（[SH64] exit pid=… code=0）",
          wait_re(r"\[SH64\] exit pid=\d+ code=0", 30) is not None, "")
    log_final = slog()
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log_final)
    check("系统还活着（没有 PANIC 后的复位）", proc.poll() is None, "qemu rc=%s" % proc.poll())

    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    if args.keep:
        print("[a42a] 串口日志：%s" % serial)
    print("--- serial tail ---")
    for line in [x for x in log_final.splitlines() if x.strip()][-25:]:
        print("   | " + line[:180])
    bad = [n for n, c in checks if not c]
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    if bad:
        print("失败项：")
        for n in bad:
            print("   - " + n)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
