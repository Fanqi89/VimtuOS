#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/apps/hello-cli/tests/hello_cli_test.py - hello-cli 的端到端验收（模板）

跑法（Windows 原生 Python；**同一时刻只跑一个 QEMU 重活**）：
    py -3 sdk/software-template/apps/hello-cli/tests/hello_cli_test.py [--img 现成夹具盘] [--keep]

断言（**一条都不许削弱**）：
  ① 夹具卷里真有 /bin/hello-cli.elf（字节数与宿主产物一致）+ /etc/hello.conf；
  ② 登录进桌面 -> 开始菜单开终端（[GUI64] ready + [APP] term opened）；
  ③ `elfrun /bin/hello-cli.elf` 之后程序**真的在 ring3 跑起来**（[HELLO-CLI] ver=1 pid=<n>）；
  ④ 目录枚举走**新 ABI**：内核打 [DIR64] open/read/close，50/51/52 不在 enosys 清单里；
     程序自己列出 / 的条目（bin/lib/etc/apps/icons 这些目录 kind=dir）；
  ⑤ 读卷里的文件：/etc/hello.conf 的字节数与内容与夹具**逐值一致**；
  ⑥ 收尾：summary rc=0 + done（进程正常退出）；
  ⑦ 全程无 PANIC / TRIPLE FAULT。

退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题。
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))    # sdk/software-template
ROOT = os.path.dirname(os.path.dirname(SDK))                     # 仓库根
sys.path.insert(0, os.path.join(SDK, "tools"))
import sdk_qemu as sq                                            # noqa: E402

ELF = os.path.join(ROOT, "build64", "sdk", "hello-cli.elf")
VAP = os.path.join(ROOT, "build64", "sdk", "hello-cli.vap")
FIXTURE = os.path.join(ROOT, "build64", "sdk", "hello_cli_test.img")
CONF_TEXT = b"hello-cli fixture conf\n"
CONF_PATH = "/etc/hello.conf"

FORBIDDEN = ("PANIC", "TRIPLE FAULT")



def ensure_artifacts(verbose=True):
    """缺产物就自己造（验收脚本要能**独立**跑起来）：ELF 走 build.sh，VAP 走 vap64_pack.py。"""
    import subprocess
    if not os.path.exists(ELF):
        print("   缺 %s -> 先跑 build.sh" % ELF)
        r = subprocess.run(["bash", os.path.join(os.path.dirname(HERE), "build.sh")],
                           cwd=ROOT, capture_output=True)
        if r.returncode != 0 or not os.path.exists(ELF):
            raise RuntimeError("构建失败：%s" % r.stderr.decode("utf-8", "replace")[-300:])
    if not os.path.exists(VAP):
        print("   缺 %s -> 先跑 vap64_pack.py" % VAP)
        r = subprocess.run(["py", "-3", os.path.join(SDK, "packaging", "vap64_pack.py"),
                            os.path.join(ROOT, "build64", "sdk", "hello-cli.bin"), VAP, "hello-cli"],
                           cwd=ROOT, capture_output=True)
        if r.returncode != 0 or not os.path.exists(VAP):
            raise RuntimeError("VAP64 打包失败：%s" % r.stderr.decode("utf-8", "replace")[-300:])


def prepare(verbose=True):
    ensure_artifacts(verbose)
    conf = os.path.join(ROOT, "build64", "sdk", "hello.conf")
    with open(conf, "wb") as f:
        f.write(CONF_TEXT)
    srcs = [
        (ELF, "/bin/hello-cli.elf", 0o755),
        (VAP, "/apps/hello-cli/hello-cli.vap", 0o755),
        (conf, CONF_PATH, 0o644),
    ]
    # 夹具尽量小、启动快：CLI 演示不需要字库/图标（列目录看到的是基础卷 + /bin + /etc + /apps）
    return sq.build_fixture(FIXTURE, srcs, verbose=verbose)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))
        return bool(cond)

    print("=== 0) 夹具盘 ===")
    if not os.path.exists(os.path.join(ROOT, "build64", "system.img")):
        sys.stderr.write("缺少 build64/system.img（先跑 bash build64.sh）\n")
        return 2
    img = args.img
    if not img:
        try:
            img = prepare()                    # 内部会在缺产物时自动 build.sh / vap64_pack.py
        except Exception as e:
            sys.stderr.write("夹具盘准备失败：%s\n" % e)
            return 2
    check("夹具盘就绪", os.path.exists(img), img)

    qemu = sq.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    port = args.port or sq.free_port()
    tmp, serial = sq.temp_serial("vimtu_sdk_cli_")
    vm = None
    log = ""
    try:
        vm = sq.QemuVm(qemu, img, port, serial, name="vimtu-sdk-cli")
        mon = sq.Monitor(port)
        print("=== 1) 引导 + 登录 + 开终端 ===")
        check("内核起来（[GUI64] ready）", sq.login_desktop(mon, vm), "")
        check("开始菜单 -> 终端（[APP] term opened）", sq.open_terminal(mon, vm), "")

        print("=== 2) 在终端里跑 /bin/hello-cli.elf ===")
        sq.type_line(mon, "elfrun /bin/hello-cli.elf", per_key=0.11)
        m = vm.wait_re(r"\[HELLO-CLI\] ver=1 pid=(\d+)", 60)
        check("[HELLO-CLI] 起来了（真进程）", m is not None, m.group(0) if m else "没等到")
        check("程序打了 hello 行", vm.wait("[HELLO-CLI] hello", 20))
        check("done 行（进程正常收尾）", vm.wait("[HELLO-CLI] done", 60))

        log = vm.log()
        mo = re.search(r"\[HELLO-CLI\] dir open path=/ ok=1 err=0", log)
        check("目录枚举 open 成功（自有 ABI 50）", mo is not None, mo.group(0) if mo else "没等到")
        mr = re.findall(r"\[DIR64\] (open|read|close)[^\n]*", log)
        check("内核 [DIR64] 打点齐全（open/read/close）",
              len(mr) > 0 and any(x.startswith("open") for x in mr) and any(x.startswith("close") for x in mr),
              "打点 %d 条" % len(mr))
        enosys = re.findall(r"\[SYSCALL\][^\n]*enosys[^\n]*", log)
        check("50/51/52 不在 enosys 清单里", all(("50" not in e and "51" not in e and "52" not in e)
                                                  for e in enosys), "enosys 行 %d 条" % len(enosys))
        ms = re.search(r"\[HELLO-CLI\] dir items=(\d+) dirs=(\d+) files=(\d+)", log)
        # 阈值：基础卷（/bin + /etc）+ 我加的 /apps → 至少 3 个目录；条目数必须 > 0
        check("枚举出目录/文件（items>0 且 dirs>=3）",
              ms is not None and int(ms.group(1)) > 0 and int(ms.group(2)) >= 3,
              ms.group(0) if ms else "没等到")
        md = re.search(r"\[HELLO-CLI\] ent name=bin kind=dir", log)
        check("bin 被识别为目录（kind=dir）", md is not None, md.group(0) if md else "没等到")
        mc = re.search(r"\[HELLO-CLI\] conf path=/etc/hello.conf bytes=(\d+) text=\"([^\"]*)\"", log)
        # 程序把换行/回车/制表折成空格（日志一行一条），所以期望串也要这么折
        want_txt = CONF_TEXT.decode().replace("\r", " ").replace("\n", " ").replace("\t", " ")
        check("读到 /etc/hello.conf 且字节数/内容一致",
              mc is not None and int(mc.group(1)) == len(CONF_TEXT) and mc.group(2) == want_txt,
              mc.group(0) if mc else "没等到")
        msum = re.search(r"\[HELLO-CLI\] summary items=(\d+) dirs=(\d+) files=(\d+) conf_bytes=(\d+) rc=0", log)
        check("summary rc=0", msum is not None, msum.group(0) if msum else "没等到")
        check("全程无 PANIC / TRIPLE FAULT", not any(f in log for f in FORBIDDEN))
    finally:
        if vm:
            vm.close()
        if not args.keep:
            for f in (os.path.join(ROOT, "build64", "sdk", "hello.conf"),):
                pass
        print("   串口日志：%s" % serial)
        print("=== 结论：%s（%d/%d）==="
              % ("PASS" if ok else "FAIL", sum(1 for _n, c in checks if c), len(checks)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
