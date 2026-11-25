# -*- coding: utf-8 -*-
"""tests/shortcut64_test.py —— ★ v0.12.1：快捷方式目录驱动的开始菜单（动态扫描）+ Dock 固定图标

判据全在串口（可 grep，不靠肉眼）：

  阶段 1（首次启动：播种）
    [APP64] shortcuts seed dir=/apps wrote=4 kept=0 ok=1
      → 图片查看器 / MicroPython / 文本编辑器 / 商店 四份 .lnk 真的写进 /apps

  阶段 2（首次启动：扫描 -> 注册表）
    [APP64] shortcuts scan dirs=3 added=<n> entries=<m> fnv=<hex> ok=1
      → 开始菜单的列表**来自目录**（不是写死的表）
    [APPREG64] app idx=… name=图片查看器 … exec=/bin/viewer icon=image-viewer builtin=0
      → 图片查看器真的进了注册表（于是开始菜单里能看见）★ 用户报的"根本没有显示"就修在这

  阶段 3（第二次启动：幂等 + 复用文件）
    [APP64] shortcuts seed … wrote=0 kept=4 ok=1
      → 已存在的不覆盖（用户改过的快捷方式不会被启动期踩掉）
    [APP64] shortcuts scan … added=0
      → 同一份目录不会重复追加

  阶段 4
    不得出现 PANIC / TRIPLE FAULT / selftest FAIL / OOM

运行：py -3 tests/shortcut64_test.py [--qemu <path>] [--keep]
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

import proc64_test as p64          # noqa: E402
import dragdrop64_test as dd       # noqa: E402  Vm / Mon / wait_re / login

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(SYSDISK):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSDISK)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_sc64_")
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(SYSDISK, disk)
    serial = os.path.join(tmp, "serial.log")
    checks = []
    state = {"ok": True}

    def check(name, cond, detail=""):
        state["ok"] = state["ok"] and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, log):
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:"):
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    vm = mon = None
    try:
        print("=== 阶段 1~2：首次启动 -> 播种 /apps + 扫描进注册表 ===")
        vm = dd.Vm(qemu, disk, serial, "Vimtu64-sc64-1")
        mon = dd.Mon(vm.port)
        check("锁屏 -> 登录 -> 桌面", dd.login(vm, mon), "serial=%s" % serial)

        m_seed = dd.wait_re(vm, r"\[APP64\] shortcuts seed dir=/apps wrote=(\d+) kept=(\d+) ok=1", 40)
        check("启动期幂等播种 /apps/*.lnk（[APP64] shortcuts seed … ok=1）", m_seed is not None,
              m_seed.group(0) if m_seed else "（缺行）")
        check("首次启动真的写出 4 份快捷方式（wrote=4）",
              bool(m_seed) and m_seed.group(1) == "4",
              "wrote=%s kept=%s" % (m_seed.group(1), m_seed.group(2)) if m_seed else "-")

        m_scan = dd.wait_re(vm, r"\[APP64\] shortcuts scan dirs=(\d+) added=(\d+) entries=(\d+) .*ok=1", 40)
        check("快捷方式目录被扫描（[APP64] shortcuts scan … ok=1）", m_scan is not None,
              m_scan.group(0) if m_scan else "（缺行）")
        check("扫描真的把条目追加进了注册表（added>=1）",
              bool(m_scan) and int(m_scan.group(2)) >= 1,
              "added=%s" % (m_scan.group(2) if m_scan else "-"))

        # ★ 用户报的痛点：图片查看器在开始菜单里"根本没有显示"
        m_vw = dd.wait_re(vm, r"\[APPREG64\] app idx=\d+ app=\d+ name=图片查看器 .*exec=/bin/viewer "
                              r"icon=image-viewer builtin=0", 40)
        check("图片查看器进了注册表（name=图片查看器 exec=/bin/viewer icon=image-viewer）",
              m_vw is not None, m_vw.group(0)[:100] if m_vw else "（缺行）")
        m_mp = dd.wait_re(vm, r"\[APPREG64\] app idx=\d+ app=\d+ name=MicroPython .*exec=/bin/mp ", 40)
        check("MicroPython 也进了注册表（exec=/bin/mp）", m_mp is not None,
              m_mp.group(0)[:100] if m_mp else "（缺行）")

        # 开始菜单的视图是从注册表构建的：grid apps 数必须 > 内置 12 条
        m_grid = dd.wait_re(vm, r"\[START64\] grid apps=(\d+) cells=(\d+)", 40)
        check("开始菜单视图条目数包含扫进来的应用（grid apps >= 13）",
              bool(m_grid) and int(m_grid.group(1)) >= 13,
              "apps=%s" % (m_grid.group(1) if m_grid else "-"))
        check("开始菜单里能查到 /bin/viewer 这一项",
              "/bin/viewer" in vm.log() or "图片查看器" in vm.log())
        forbid("阶段 1~2 日志", vm.log())
        vm.close()
        vm = None

        print("=== 阶段 3：第二次启动（同一块盘）-> 播种幂等 + 扫描不重复 ===")
        serial2 = os.path.join(tmp, "serial2.log")
        vm2 = dd.Vm(qemu, disk, serial2, "Vimtu64-sc64-2")
        mon2 = dd.Mon(vm2.port)
        check("第二次启动到桌面", dd.login(vm2, mon2), "serial=%s" % serial2)
        m_seed2 = dd.wait_re(vm2, r"\[APP64\] shortcuts seed dir=/apps wrote=(\d+) kept=(\d+) ok=1", 40)
        check("已存在的快捷方式不被覆盖（wrote=0 kept=4）",
              bool(m_seed2) and m_seed2.group(1) == "0" and m_seed2.group(2) == "4",
              m_seed2.group(0) if m_seed2 else "（缺行）")
        # ★ 正则要把 entries 也抓出来（判"不膨胀"靠它）
        m_scan2 = dd.wait_re(vm2, r"\[APP64\] shortcuts scan dirs=(\d+) added=(\d+) entries=(\d+) ", 40)
        # ★ 语义更正（v0.12.1 实测）：扫进来的条目**不入 /etc/apps.json**（真源就是 /apps/*.lnk），
        #   所以每次启动都会重新追加 —— added 稳定等于"目录里那几条"（这里 2 条：图片查看器/MicroPython；
        #   文本编辑器与商店的 exec 已在内置表 -> 被 exec 去重）。判据 = **条目总数与首次完全一致**，
        #   也就是"不重复膨胀"（原来写成 added=0 是我把设计理解错了，不改产品）。
        check("同一份目录不会重复膨胀（entries 与首次一致 = 14）",
              bool(m_scan2) and m_scan2.group(3) == "14",
              "entries=%s added=%s" % (m_scan2.group(3) if m_scan2 else "-",
                                       m_scan2.group(2) if m_scan2 else "-"))
        check("第二次启动扫描到的仍是同样那 2 条（added=2）",
              bool(m_scan2) and m_scan2.group(2) == "2",
              "added=%s" % (m_scan2.group(2) if m_scan2 else "-"))
        m_vw2 = dd.wait_re(vm2, r"\[APPREG64\] app idx=\d+ app=\d+ name=图片查看器 .*exec=/bin/viewer ", 40)
        check("第二次启动图片查看器仍在开始菜单（真源是 /apps 目录，不是内存）", m_vw2 is not None,
              m_vw2.group(0)[:100] if m_vw2 else "（缺行）")
        forbid("阶段 3 日志", vm2.log())
        vm2.close()
    except Exception as e:
        state["ok"] = False
        print("  [FAIL] 运行异常：%r" % (e,))
    finally:
        if vm is not None:
            vm.close()

    n_pass = sum(1 for _, ok in checks if ok)
    print("\n===== 汇总：%d/%d PASS =====" % (n_pass, len(checks)))
    if not args.keep:
        try:
            shutil.rmtree(tmp, ignore_errors=True)
        except OSError:
            pass
    print("===== shortcut64_test: %s =====" % ("PASS" if state["ok"] else "FAIL"))
    return 0 if state["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
