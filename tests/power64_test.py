#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/power64_test.py - 电源管理验收：**点"关机"必须真的断电**（不是重启、不是 halt）

本批修的缺陷（v0.9.37 之前的真实行为）：
  kernel/gui64.cpp 的 sys_shutdown64() **从不调用** acpi_poweroff64()，
  而是硬编码 QEMU 专有口 `outw(0x604, 0x2000)`；失败后又写 8042 的 0xFE ——
  那是 **CPU 复位**（重启），不是断电。所以：
    * QEMU 上碰巧能断（它的 S5 SLP_TYPa 正好是 0，0x2000 = SLP_EN|type0）；
    * VMware / 真机上大概率**变成重启**（0x604 不是它们的 PM1a_CNT_BLK）。
  同时 settings64.cpp 的日志写的是 "via=spec (ACPI S5 -> acpi_poweroff64)" ——
  **打点与事实不符**，这一条也一并纠正。

本测试的判据（只认现场证据，不看代码）：
  ① `[ACPI64] power s5_slp_typa=<n> src=dsdt|default pm1a_cnt=0x<port> ...`
     —— 睡眠类型来自固件（DSDT 的 _S5）或明确标注为默认值，端口来自 FADT。
  ② `[SHUTDOWN] acpi s5 pm1a_cnt=0x<port> slp_typa=<n> src=<src>`
     —— 关机路径用的端口**等于** ① 里 FADT 给的那个（不是硬编码）。
  ③ `[ACPI64] poweroff pm1a_cnt=0x<port> val=0x<..> slp_typa=<n> len=<..>`
     —— PM1a_CNT 真的被写下去了，且 val 的 bit13(SLP_EN)=1。
  ④ QEMU 进程在 N 秒内**自己退出**（真的断电；halt 不会退出，重启会再启动一轮）。
  ⑤ 串口里 `[ACPI64] rsdp=` 只出现 **1 次** —— 没有第二轮启动（= 不是重启）。
  ⑥ 串口里**没有** `[SHUTDOWN] FAILED -> halt` —— 阶梯没有走到底。

★ 反向对照（本测试同时能抓）：旧代码在 ⑤/⑥ 之外还会打 `[SHUTDOWN] sys_shutdown64: ACPI 0x604`，
  且**不会**有 ②（那行不存在）—— 所以 ② 的缺失本身就是旧代码的指纹。

用法：py -3 tests/power64_test.py [--img build64/system.img] [--qemu 路径] [--port 5672]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
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

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

# 断电阶梯最多 4 级（见 kernel/gui64.cpp sys_shutdown64），每级忙等 1.5 s；
# 给 25 s 余量已经非常宽松（正常一台 QEMU 点了 poweroff 是毫秒级退出）。
POWEROFF_EXIT_TIMEOUT = 25.0


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


class Monitor(object):
    """QEMU monitor（telnet）最小客户端：只要 sendkey。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)


class Vm(object):
    """一次 QEMU 引导。

    ★ 刻意**不加** `-no-reboot`：加了之后"重启"和"断电"在宿主侧都表现为进程退出，
    判据就分不开了。不加的话：真断电 -> QEMU 退出；被 8042 复位 -> guest 重新引导，
    串口里会出现**第二轮** `[ACPI64] rsdp=`（这正是本批要抓的旧缺陷形态）。
    """

    def __init__(self, qemu, img, port, name="vimtu-power64", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_power_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                # 进程已经退出：再读一次，别把最后几 KB 漏掉
                return needle in self.log()[since:]
            time.sleep(0.3)
        return False

    def wait_exit(self, timeout):
        """等 QEMU 自己退出（= 真的断电了）。返回 (exited, returncode)。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.proc.poll() is not None:
                return True, self.proc.returncode
            time.sleep(0.25)
        return False, None

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


FAILS = []


def check(name, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    line = "[%s] %s" % (tag, name)
    if detail:
        line += "  |  " + str(detail)
    print(line)
    if not ok:
        FAILS.append(name)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5672)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统镜像 %s（先跑全量构建）" % args.img)
        return 2

    print("=== 0) 启动 QEMU（★ 不加 -no-reboot：重启必须能被看见）===")
    vm = Vm(qemu, args.img, args.port)
    print("    serial = %s" % vm.serial)
    try:
        mon = vm.wait_monitor()
        if not vm.wait_log("[LOCK64] bg blur ready", 180):
            print("环境问题：等不到锁屏就绪（看 %s）" % vm.serial)
            return 2
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=1.5)
        if not vm.wait_log("[GUI64] ready", 120):
            print("环境问题：进不了桌面（看 %s）" % vm.serial)
            return 2
        log = vm.log()

        # ---------- ① 开机期：ACPI 电源信息行 ----------
        print("=== 1) 开机期 ACPI 电源信息（睡眠类型来自固件？端口来自 FADT？）===")
        m_pow = re.search(
            r"\[ACPI64\] power s5_slp_typa=(\d+) src=(dsdt|default) pm1a_cnt=(0x[0-9a-fA-F]+)"
            r" pm1_cnt_len=(\d+) reset_reg=(\S+) reset_val=(0x[0-9a-fA-F]+) dsdt=(\S+) dsdt_len=(\d+)",
            log)
        check("① [ACPI64] power 行存在（s5_slp_typa/src/pm1a_cnt 全在）",
              m_pow is not None, m_pow.group(0) if m_pow else "未出现")
        if not m_pow:
            print("\n没有 ACPI 电源行就无法继续判电源路径 —— 先看串口。")
            return 1
        slp_typa = int(m_pow.group(1))
        slp_src = m_pow.group(2)
        pm_port = m_pow.group(3)
        dsdt = m_pow.group(7)
        check("① SLP_TYPa 落在合法区间 0..7", 0 <= slp_typa <= 7, "slp_typa=%d" % slp_typa)
        check("① PM1a_CNT_BLK 是合法端口（非 0）", pm_port.lower() != "0x0000000000000000",
              "pm1a_cnt=%s" % pm_port)
        check("① DSDT 地址已记录（src=dsdt 时必须有）",
              (slp_src == "default") or (dsdt.lower() != "0x0000000000000000"),
              "src=%s dsdt=%s dsdt_len=%s" % (slp_src, dsdt, m_pow.group(8)))

        # ---------- ② 触发桌面关机 ----------
        print("=== 2) 桌面点关机（Win 键 -> 开始菜单数字 1 = 终端 -> poweroff）===")
        base = len(vm.log())
        mon.key("meta_l", wait=1.2)
        check("② Win 键打开开始菜单", vm.wait_log("[START64] open why=win-key", 10))
        mon.key("1", wait=1.6)
        check("② 数字快捷键 1 打开终端", vm.wait_log("[APP] term opened", 12))
        for k in ("p", "o", "w", "e", "r", "o", "f", "f"):
            mon.key(k, wait=0.18)
        mon.key("ret", wait=0.2)

        check("② 走的是 UI 关机路径（[UI] shutdown anim start）",
              vm.wait_log("[UI] shutdown anim start", 12))

        # ---------- ③ 关机用的是 FADT 端口，不是硬编码 0x604 ----------
        print("=== 3) 关机路径的证据（端口必须等于 FADT 给的那个）===")
        got_out = vm.wait_log("[SHUTDOWN] acpi s5 pm1a_cnt=", 8)
        check("③ [SHUTDOWN] acpi s5 打点存在（旧代码这里是 'ACPI 0x604'）", got_out)
        m_sd = re.search(r"\[SHUTDOWN\] acpi s5 pm1a_cnt=(0x[0-9a-fA-F]+) slp_typa=(\d+) src=(\w+)",
                         vm.log())
        check("③ 打点字段完整（pm1a_cnt/slp_typa/src）", m_sd is not None,
              m_sd.group(0) if m_sd else "未出现")
        if m_sd:
            check("③ ★ 关机端口 == FADT 的 PM1a_CNT_BLK（不是硬编码 0x604）",
                  m_sd.group(1).lower() == pm_port.lower(),
                  "shutdown=%s fadt=%s" % (m_sd.group(1), pm_port))
            check("③ ★ 睡眠类型与开机期解析的一致",
                  int(m_sd.group(2)) == slp_typa,
                  "shutdown=%s acpi=%d" % (m_sd.group(2), slp_typa))

        # ---------- ④ PM1a_CNT 真的被写了 ----------
        print("=== 4) PM1a_CNT 寄存器写入（SLP_EN 必须为 1）===")
        # ★ 竞态：③ 的 wait_log 一看到 s5 行就返回，而 [ACPI64] poweroff 行是**紧接着**才打的，
        #   所以这里必须再等一次，不能直接对当前日志做正则（上一版就是这么假红的）。
        vm.wait_log("[ACPI64] poweroff pm1a_cnt=", 10)
        m_w = re.search(
            r"\[ACPI64\] poweroff pm1a_cnt=(0x[0-9a-fA-F]+) val=(0x[0-9a-fA-F]+) "
            r"slp_typa=(\d+) len=(\d+)", vm.log())
        check("④ [ACPI64] poweroff 打点存在（acpi_poweroff*() 真被调用了）", m_w is not None,
              m_w.group(0) if m_w else "未出现")
        if m_w:
            val = int(m_w.group(2), 16)
            check("④ 写的是 FADT 端口", int(m_w.group(1), 16) == int(pm_port, 16),
                  "write=0x%x fadt=0x%x" % (int(m_w.group(1), 16), int(pm_port, 16)))
            check("④ ★ val 的 bit13 = SLP_EN = 1", bool(val & (1 << 13)), "val=0x%x" % val)
            check("④ ★ val 的 bit12..10 == SLP_TYPa",
                  ((val >> 10) & 0x7) == int(m_w.group(3)),
                  "val=0x%x slp_typa=%s" % (val, m_w.group(3)))

        # ---------- ⑤ 真的断电：QEMU 自己退出 ----------
        print("=== 5) 真的断电（QEMU 进程必须自己退出）===")
        exited, rc = vm.wait_exit(POWEROFF_EXIT_TIMEOUT)
        check("⑤ ★ QEMU 在 %.0f s 内自己退出（halt 不会退、重启会再启动）" % POWEROFF_EXIT_TIMEOUT,
              exited, "exited=%s rc=%s" % (exited, rc))

        # ---------- ⑤b：第 ① 级就该断电 ----------
        # 如果固件忽略了 _S5 解析出来的类型，阶梯会退到第二级并打 "ignored (still running)" ——
        # 那说明"从 DSDT 读睡眠类型"在这台机器上没赚到，必须如实红。
        check("⑤b ★ 第 ① 级（固件 _S5 的类型）一次就断电（没有 ignored 行）",
              "[SHUTDOWN] s5 slp_typa=" not in vm.log())

        # ---------- ⑥ 不是重启 ----------
        print("=== 6) 不是重启、也没把阶梯走到底 ===")
        time.sleep(1.0)
        final = vm.log()
        n_boot = final.count("[ACPI64] rsdp=")
        check("⑥ ★ 串口里 [ACPI64] rsdp= 只出现 1 次（没有第二轮启动 = 不是重启）",
              n_boot == 1, "rsdp 行数=%d" % n_boot)
        check("⑥ ★ 没有走到阶梯底部（[SHUTDOWN] FAILED -> halt 不存在）",
              "[SHUTDOWN] FAILED -> halt" not in final)
        check("⑥ 没有退回 legacy 兼容口（QEMU 上 FADT 端口 == 0x604，不该多写一次）",
              "[SHUTDOWN] legacy compat port 0x604" not in final)
        check("⑥ 没有 PANIC", "PANIC" not in final)

        if args.keep:
            print("串口留档：%s" % vm.serial)
    finally:
        vm.close()

    print("")
    if FAILS:
        print("FAILED %d 项：%s" % (len(FAILS), "; ".join(FAILS)))
        return 1
    print("PASS：电源管理 ①~⑥ 全部通过（关机走 FADT+DSDT，真断电，不是重启）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
