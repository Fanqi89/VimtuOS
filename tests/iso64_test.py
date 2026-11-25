# -*- coding: utf-8 -*-
"""iso64_test.py —— ★ v0.11.8 Ring 3 真隔离层验收（硬件隔离位 + 地址空间审计 + 活进程审计）。

判据全在串口（可 grep，不靠肉眼）：

  阶段 1  硬件隔离位（启动期就置位）
    [ISO64] hw wp=1 smep=<0|1> smep_supported=<0|1> smap_supported=<0|1> smap_on=0 cr0=<hex> cr4=<hex> ok=1
      → WP 必须为 1；CPU 支持 SMEP 时 smep 必须为 1；**smap_on=0 是如实**（本版不开，理由见 iso64.h）

  阶段 2  如实标注"还没做的那层"
    [ISO64] mode kpti=0 smap=0 reason=…
      → 这一行存在才说明"审计通过"没被冒充成"KPTI 已实现"

  阶段 3  启动期自检（6 位）
    [ISO64] selftest PASS mask=63
      → ① CR0.WP ② CR4.SMEP ③ 内核地址被用户指针闸门拒绝 ④ 用户地址被接受
        ⑤⑥ 内核地址空间 + 所有进程地址空间审计零违规

  阶段 4  内核地址空间审计（数出来的数）
    [ISO64] audit pid=- space=<hex> khigh=<n> ident=<n> uwin=<n> outside=0 us_violation=0 ok=1
      → khigh>0（内核高半区确实走到了）、ident>0（前 4GB 恒等映射确实走到了）、outside=0

  阶段 5  活进程审计（在终端里敲 `iso`）
    [ISO64] status wp=1 …
    [ISO64] audit pid=<n> … uwin=<n>>=1 us_violation=0 ok=1
    [ISO64] audit all spaces=<n> … violations=0 ok=1
      → 有 ring3 进程时，它的用户窗口里必须有页(uwin>=1)且**没有任何用户可访问的内核项**

  阶段 6  不得出现 PANIC / TRIPLE FAULT / selftest FAIL / OOM

运行：py -3 tests/iso64_test.py [--qemu <path>] [--keep]
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


def type_cmd(mon, text, per_key=0.09):
    """敲命令 + 回车（大写要映射成 shift-<小写>：QEMU sendkey 不认大写，实测踩过）。"""
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
             ">": "shift-dot", "<": "shift-comma", "=": "equal", ":": "shift-semicolon"}
    for ch in text:
        if ch in names:
            mon.key(names[ch], wait=per_key)
        elif ch.isdigit():
            mon.key(ch, wait=per_key)
        elif ch.isalpha():
            mon.key(("shift-" + ch.lower()) if ch.isupper() else ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持该字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.25)


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_iso_")
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
        print("=== 阶段 1~4：启动 -> 硬件隔离位 / 如实标注 / 自检 / 内核地址空间审计 ===")
        vm = dd.Vm(qemu, disk, serial, "Vimtu64-iso")
        mon = dd.Mon(vm.port)
        check("锁屏 -> 登录 -> 桌面", dd.login(vm, mon), "serial=%s" % serial)

        m_hw = dd.wait_re(vm, r"\[ISO64\] hw wp=(\d) smep=(\d) smep_supported=(\d) smap_supported=(\d) "
                               r"smap_on=(\d) cr0=([0-9A-Fa-f]+) cr4=([0-9A-Fa-f]+) ok=(\d)", 40)
        check("[ISO64] hw 行存在（启动期就打了）", m_hw is not None,
              m_hw.group(0) if m_hw else "（缺行）")
        check("CR0.WP=1（内核也不能写只读页）", bool(m_hw) and m_hw.group(1) == "1",
              "wp=%s" % (m_hw.group(1) if m_hw else "-"))
        smep_sup = m_hw.group(3) if m_hw else "0"
        smep_on = m_hw.group(2) if m_hw else "0"
        check("CR4.SMEP 与 CPU 支持一致（支持则必须开）",
              (smep_sup == "0") or (smep_sup == "1" and smep_on == "1"),
              "supported=%s on=%s" % (smep_sup, smep_on))
        check("SMAP 如实：smap_on=0（本版不开）", bool(m_hw) and m_hw.group(5) == "0",
              "smap_supported=%s" % (m_hw.group(4) if m_hw else "-"))

        m_mode = dd.wait_re(vm, r"\[ISO64\] mode kpti=(\d) smap=0 reason=", 20)
        check("[ISO64] mode 行存在（如实标注 KPTI/SMAP 现状）", m_mode is not None,
              m_mode.group(0)[:100] if m_mode else "（缺行）")
        # ★ v0.11.9：本测试全程走 BIOS 路径（自有页表）→ KPTI 必须真的启用
        check("KPTI 真的启用（[ISO64] mode kpti=1）", bool(m_mode) and m_mode.group(1) == "1",
              "kpti=%s" % (m_mode.group(1) if m_mode else "-"))

        # ★ v0.11.9：KPTI 共享入口区的建立行（页表/IDT/蹦床的物理证据都在里面）
        m_kinit = dd.wait_re(vm, r"\[KPTI64\] init ok area=0000000180000000 blob_pa=([0-9A-Fa-f]+) "
                                  r"pd=([0-9A-Fa-f]+) pt=([0-9A-Fa-f]+) idt=([0-9A-Fa-f]+) "
                                  r"entry_rsp=([0-9A-Fa-f]+) tramp=(\d+) kpti=1", 25)
        check("[KPTI64] init ok（共享入口区建立、kpti=1）", m_kinit is not None,
              m_kinit.group(0)[:120] if m_kinit else "（缺行）")
        check("共享入口区装了 50 条向量蹦床（tramp=50）",
              bool(m_kinit) and m_kinit.group(6) == "50",
              "tramp=%s" % (m_kinit.group(6) if m_kinit else "-"))
        m_kst = dd.wait_re(vm, r"\[KPTI64\] selftest (PASS|FAIL) mask=(\d+) bits=(\d+)", 25)
        check("KPTI 自检 PASS mask=63（6 位：映射/IDTR/门目标/栈/数据槽/内核栈镜像）",
              bool(m_kst) and m_kst.group(1) == "PASS" and m_kst.group(2) == "63",
              m_kst.group(0) if m_kst else "（缺行）")

        m_st = dd.wait_re(vm, r"\[ISO64\] selftest (PASS|FAIL) mask=(\d+)", 25)
        check("隔离自检 PASS mask=63", bool(m_st) and m_st.group(1) == "PASS" and m_st.group(2) == "63",
              m_st.group(0) if m_st else "（缺行）")

        m_k = dd.wait_re(vm, r"\[ISO64\] audit pid=- space=([0-9A-Fa-f]+) khigh=(\d+) ident=(\d+) "
                              r"uwin=(\d+) outside=(\d+) us_violation=(\d+)", 25)
        check("内核地址空间审计行存在", m_k is not None, m_k.group(0) if m_k else "（缺行）")
        check("审计真的走到了内核高半区（khigh>0）", bool(m_k) and int(m_k.group(2)) > 0,
              "khigh=%s" % (m_k.group(2) if m_k else "-"))
        check("审计真的走到了前 4GB 恒等映射（ident>0）", bool(m_k) and int(m_k.group(3)) > 0,
              "ident=%s" % (m_k.group(3) if m_k else "-"))
        check("用户窗口外没有任何用户可访问项（outside=0）", bool(m_k) and m_k.group(5) == "0",
              "outside=%s" % (m_k.group(5) if m_k else "-"))
        check("内核地址空间零违规（us_violation=0）", bool(m_k) and m_k.group(6) == "0",
              "violation=%s" % (m_k.group(6) if m_k else "-"))
        # ★ v0.11.9：共享入口区（PDPT[6]，6 GiB..7 GiB）必须被审计**认出来**（entry>=1），
        #   而不是被算成"窗口外越权映射"。这一条同时证明那张 PD 是我们自己的。
        m_ke = dd.wait_re(vm, r"\[ISO64\] audit pid=- .*outside=0 us_violation=0 why=\S+ ok=1 "
                              r"entry=(\d+) fbwin=\d+ kind=kern", 15)
        check("共享入口区被审计认出来（entry>=1，kind=kern）",
              bool(m_ke) and int(m_ke.group(1)) >= 1,
              m_ke.group(0)[-46:] if m_ke else "（缺行）")
        forbid("阶段 1~4 日志", vm.log())

        print("=== 阶段 5：活进程审计（终端里敲 iso）===")
        since = len(vm.log())
        mon.key("meta_l", wait=1.0)
        mon.key("1", wait=2.5)                     # 开始菜单 1 = 终端
        check("终端能打开", vm.wait_log("[TERM]", 25, since) or vm.wait_log("[APP] term opened", 25, since))
        time.sleep(1.0)
        n0 = len(vm.log())
        type_cmd(mon, "iso")
        m_s = dd.wait_re(vm, r"\[ISO64\] status wp=(\d) smep=(\d) smap=0\(supported=\d\) kpti=(\d) ",
                         25, n0)
        check("`iso` 命令输出状态行（[ISO64] status …）", m_s is not None,
              m_s.group(0) if m_s else "（缺行）")
        check("`iso` 里 KPTI 状态仍是 1", bool(m_s) and m_s.group(3) == "1",
              "kpti=%s" % (m_s.group(3) if m_s else "-"))
        m_all = dd.wait_re(vm, r"\[ISO64\] audit all spaces=(\d+) procs=(\d+) slots=\d+ violations=(\d+) ok=(\d)",
                           25, n0)
        check("`iso` 触发一次全地址空间审计（audit all … violations=0 ok=1）",
              bool(m_all) and m_all.group(3) == "0" and m_all.group(4) == "1",
              m_all.group(0) if m_all else "（缺行）")
        # ★ 活进程判据用 **probe**：`iso` 会自己造一个真进程（proc64_create64）、映一页用户页、
        #   审计它的地址空间、再销毁 —— 本机终端 shell 不是独立进程（procs=0），拿外部 app 靠不住。
        m_probe = dd.wait_re(vm, r"\[ISO64\] probe pid=(\d+) map=(\d) range=(\d) us_violation=(\d) "
                                  r"pa=([0-9A-Fa-f]+) kpti_u=(\d) u_violation=(\d) ok=(\d)",
                             25, n0)
        check("probe 造出真进程并映了用户页（[ISO64] probe … map=1 range=1）",
              bool(m_probe) and m_probe.group(2) == "1" and m_probe.group(3) == "1",
              m_probe.group(0) if m_probe else "（缺行）")
        check("probe 进程内核表零违规（us_violation=0）",
              bool(m_probe) and m_probe.group(4) == "0", "violation=%s" % (m_probe.group(4) if m_probe else "-"))
        check("probe 进程**用户表**零违规（u_violation=0）",
              bool(m_probe) and m_probe.group(7) == "0",
              "kpti_u=%s u_violation=%s" % ((m_probe.group(6), m_probe.group(7)) if m_probe else ("-", "-")))
        check("probe 整体 ok=1", bool(m_probe) and m_probe.group(8) == "1",
              m_probe.group(0) if m_probe else "（缺行）")
        # `iso` 里那条 audit 行必须**按 pid 打出** probe 进程的窗口页数（uwin>=1 才算数）
        m_pid = dd.wait_re(vm, r"\[ISO64\] audit pid=(\d+) space=[0-9A-Fa-f]+ khigh=\d+ ident=\d+ "
                               r"uwin=(\d+) outside=0 us_violation=0", 15, n0)
        check("活进程地址空间审计真的走到了用户窗口（uwin>=1）",
              bool(m_pid) and int(m_pid.group(2)) >= 1,
              m_pid.group(0)[:110] if m_pid else "（缺行）")
        # ★ v0.11.9（KPTI）：同一个探针进程的**用户表**也要被审 —— 判据是反的：
        #   khigh 必须 0（ring3 跑着的时候，那套地址空间里根本没有内核高半区）。
        m_pu = dd.wait_re(vm, r"\[ISO64\] audit pid=(\d+) space=[0-9A-Fa-f]+ khigh=(\d+) ident=\d+ "
                              r"uwin=(\d+) outside=0 us_violation=(\d+) .*kind=user", 15, n0)
        check("用户表审计：khigh=0（ring3 的地址空间里没有内核高半区）",
              bool(m_pu) and m_pu.group(2) == "0",
              m_pu.group(0)[:120] if m_pu else "（缺行）")
        check("用户表审计：用户窗口里确实有页（uwin>=1），且零违规",
              bool(m_pu) and int(m_pu.group(3)) >= 1 and m_pu.group(4) == "0",
              m_pu.group(0)[-60:] if m_pu else "（缺行）")
        # 建好探针进程后再跑一次自检：这时第 ④ 位（用户窗口可用）才真正被验到
        m_st2 = dd.wait_re(vm, r"\[ISO64\] selftest (PASS|FAIL) mask=(\d+) .*wp_leak=(\d)", 20, n0)
        check("`iso` 里的自检仍 PASS mask=63，且稳态下不再有 WP 泄漏（wp_leak=0）",
              bool(m_st2) and m_st2.group(1) == "PASS" and m_st2.group(2) == "63" and m_st2.group(3) == "0",
              m_st2.group(0)[:130] if m_st2 else "（缺行）")
        forbid("阶段 5 日志", vm.log())

        # ==================== 阶段 6：-cpu max（有 SMEP/SMAP 位的 CPU）====================
        # 默认 qemu64 没有 SMEP/SMAP 位（阶段 1 如实报 smep_supported=0）。
        # 这里换一个**带这些位的 CPU 模型**再启一次：SMEP 必须真的被打开（smep_supported=1 & smep=1），
        # 而 SMAP 仍然如实保持关闭（本版不开）。跑不起来就如实记 UNVERIFIED，不算产品通过。
        print("=== 阶段 6：-cpu max 启动 -> SMEP 真的能打开 ===")
        vm.close()
        disk2 = os.path.join(tmp, "disk_smep.img")
        shutil.copyfile(SYSDISK, disk2)
        serial2 = os.path.join(tmp, "serial2.log")
        port2 = None
        try:
            import fs_tree_test as fst
            port2 = fst.free_port()
            base = fst.qemu_args(qemu, [disk2], serial2, port2, "Vimtu64-iso-smep")
            args2 = base[:1] + ["-cpu", "max,la57=off,umip=off"] + base[1:]
            import subprocess
            proc2 = subprocess.Popen(args2, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
            vm2 = dd.Vm.__new__(dd.Vm)
            vm2.serial, vm2.out, vm2.proc, vm2.port = serial2, serial2 + ".qemu.txt", proc2, port2
            time.sleep(1.5)
            mon2 = dd.Mon(port2)
            if dd.login(vm2, mon2):
                m2 = dd.wait_re(vm2, r"\[ISO64\] hw wp=(\d) smep=(\d) smep_supported=(\d) smap_supported=(\d) "
                                     r"smap_on=(\d)", 40)
                check("-cpu max：CPU 报告支持 SMEP（smep_supported=1）", bool(m2) and m2.group(3) == "1",
                      m2.group(0) if m2 else "（缺行）")
                check("-cpu max：CR4.SMEP 真的被打开（smep=1，内核不能执行用户页）",
                      bool(m2) and m2.group(2) == "1", "smep=%s" % (m2.group(2) if m2 else "-"))
                check("-cpu max：SMAP 仍然如实关闭（smap_on=0）", bool(m2) and m2.group(5) == "0",
                      "smap_supported=%s" % (m2.group(4) if m2 else "-"))
                m2st = dd.wait_re(vm2, r"\[ISO64\] selftest (PASS|FAIL) mask=(\d+)", 25)
                check("-cpu max：隔离自检仍 PASS mask=63",
                      bool(m2st) and m2st.group(1) == "PASS" and m2st.group(2) == "63",
                      m2st.group(0) if m2st else "（缺行）")
            else:
                check("-cpu max 能启动到桌面（否则 SMEP 这项 UNVERIFIED）", False,
                      "serial2=%s（记 UNVERIFIED，不当通过）" % serial2)
            vm2.close()
        except Exception as e2:
            check("-cpu max 启动 / 探测（UNVERIFIED）", False, repr(e2))

    except Exception as e:
        state["ok"] = False
        print("  [FAIL] 运行异常：%r" % (e,))
    finally:
        if vm is not None:
            vm.close()

    n_pass = sum(1 for _, ok in checks if ok)
    print("\n===== 汇总：%d/%d PASS =====" % (n_pass, len(checks)))
    for nm, ok in checks:
        if not ok:
            print("  FAIL: %s" % nm)
    if args.keep:
        print("现场保留：%s" % tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if state["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
