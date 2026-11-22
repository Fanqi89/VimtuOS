"""tests/version64_test.py —— ★ 发版第 1 条硬门禁的**串口**证据（`docs/发布流程.md` §2）

判据（三者必须一致）：
  ① `build64.sh` 的 `VIMTUOS_VERSION="x.y.z"`（唯一真源，纯 MAJOR.MINOR.PATCH）
  ② 系统卷里的内核二进制 `build64/kernel64_os.bin` 内嵌串（宿主侧独立解析，逐字节）
  ③ **客户机串口**上 `VimtuOS <版本> x86_64`（终端里 `cat /proc/version`；内核合成 /proc/version 用
     编译期宏 VIMTUOS_VERSION_STR，ring3 cat 写 fd=1 = 串口 + 屏幕，见 kernel/syscall64.cpp）

另外两条反例（同等重要）：
  * 旧版本串在**内核二进制里 0 次**（证明不是"标签是新的、镜像里还是旧的"）
  * 串口上不许出现 `[PANIC64] stop=`

为什么必须有 ③：v0.3.3-beta15 / v0.4.1-beta18 两次都只改了标签，镜像内嵌串还是上一版。
只看 tag 或只看 README 一定会漏；只有**从客户机跑出来的串**才证明"发出去的那个二进制"是这个版本。

用法：py -3 tests/version64_test.py [--qemu PATH]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（缺 qemu / 缺盘）
"""
import argparse
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh                                            # noqa: E402
import deco64_test as D                                          # noqa: E402（复用启动/开终端/读日志）
import install_persist64_test as P                               # noqa: E402

DISK = os.path.join(ROOT, "build64", "sysdisk.img")
KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")
BUILD_SH = os.path.join(ROOT, "build64.sh")

ok = True
checks = []


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    checks.append((name, bool(cond)))
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def build_version():
    src = open(BUILD_SH, encoding="utf-8", errors="replace").read()
    hits = re.findall(r'^VIMTUOS_VERSION="([^"]+)"', src, re.M)
    return hits[0] if len(hits) == 1 else None          # 多于一条 = 唯一真源被破坏


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    args = ap.parse_args()

    qemu = D.find_qemu(args.qemu)
    if not qemu or not os.path.exists(DISK) or not os.path.exists(KERNEL):
        sys.stderr.write("环境问题：qemu=%r disk=%r kernel=%r\n"
                         % (qemu, os.path.exists(DISK), os.path.exists(KERNEL)))
        return 2

    ver = build_version()
    print("=== VimtuOS 发版第 1 条硬门禁（版本串三处一致）===")
    print("    build64.sh VIMTUOS_VERSION = %r" % ver)
    check("① build64.sh 里**只有一条** VIMTUOS_VERSION（唯一真源护栏）", bool(ver), repr(ver))
    if not ver:
        return 1
    check("① 版本号是纯 MAJOR.MINOR.PATCH（不带 -beta 后缀）",
          re.fullmatch(r"\d+\.\d+\.\d+", ver) is not None, ver)

    kb = open(KERNEL, "rb").read()
    n_new = kb.count(b"VimtuOS " + ver.encode())
    n_term = kb.count(b"VimtuOS Terminal v" + ver.encode())
    check("② 内核二进制里内嵌 'VimtuOS <ver>'（>=3 次：关于页 + 设置页 + 构建串）",
          n_new >= 3, "%d 次" % n_new)
    check("② 内核二进制里内嵌 'VimtuOS Terminal v<ver>'（终端 banner）", n_term >= 1, "%d 次" % n_term)

    # 旧串：从 git tag 里取最近的一个已发布版本，要求它在内核里 0 次
    old = os.popen('git -C "%s" tag --sort=-creatordate' % ROOT).read().split()
    old = [t for t in old if re.fullmatch(r"v\d+\.\d+\.\d+", t) and t != "v" + ver]
    if old:
        ov = old[0][1:]
        n_old = kb.count(b"VimtuOS " + ov.encode()) + kb.count(b"VimtuOS Terminal v" + ov.encode())
        check("② 上一个已发布版本 %s 的内嵌串在内核里 0 次" % old[0], n_old == 0, "%d 次" % n_old)

    work = tempfile.mkdtemp(prefix="vimtu64_ver_")
    img = os.path.join(work, "disk.img")
    serial = os.path.join(work, "run.log")
    shutil.copyfile(DISK, img)
    log_fn = lambda: D.log_of(serial)                              # noqa: E731
    proc = None
    try:
        port = qh.free_port()
        proc, mon = D.start(qemu, img, serial, port)
        if not qh.wait_desktop(mon, log_fn, proc, timeout=400, login_timeout=460):
            check("③ 起桌面", False, "超时")
            raise SystemExit(1)
        if not D.open_terminal_dock(log_fn, mon):
            check("③ 打开终端（[APP] term opened）", False, "Dock 入口没成功")
            raise SystemExit(1)
        before = len(log_fn())
        # ★ 为什么不用终端自带的 `cat`/`ver`/`uname`：那三个是**内核侧 builtin**，只往终端窗口里画字
        #   （ts_puts），串口一个字都没有（实测：`cat /proc/version` 敲下去串口 0 新行）。
        #   要拿到串口证据只能用 **ring3 程序**：它的 fd=1 = 串口 + 屏幕。
        #   `run PATH ARG...` 会把余下 token 当子进程 argv，所以这条命令等价于 busybox 的 cat。
        want = re.compile(r"VimtuOS %s x86_64" % re.escape(ver))
        got = None
        tried = []
        for cmd in ("run /bin/busybox cat /proc/version", "run /bin/cat /proc/version"):
            tried.append(cmd)
            before = len(log_fn())
            P.type_line(mon, cmd)
            t0 = time.time()
            while time.time() - t0 < 45:
                seg = log_fn()[before:]
                got = want.search(seg)
                if got:
                    break
                if "[PANIC64] stop=" in seg:
                    break
                time.sleep(0.5)
            if got:
                break
        seg = log_fn()
        check("③ 串口出现客户机自报的 `VimtuOS %s x86_64`（ring3 cat 读 /proc/version，写 fd=1）" % ver,
              got is not None, (got.group(0) if got else "试过 %s，都没等到" % tried))
        check("③ 全程无 PANIC", "[PANIC64] stop=" not in seg)
        check("③ 串口里不出现旧串（'VimtuOS Terminal v' + 旧版本）",
              all(("VimtuOS Terminal v" + t[1:]) not in seg for t in old), "")
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
        shutil.rmtree(work, ignore_errors=True)

    npass = sum(1 for _, c in checks if c)
    print("\n=== RESULT: %s === checks=%d ok=%d fail=%d"
          % ("PASS" if ok else "FAIL", len(checks), npass, len(checks) - npass))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
