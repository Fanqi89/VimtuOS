#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/mem4g64_test.py —— v0.11.0 内存扩容端到端验收（4GB 不再被夹成 2GB）

## 这个测试要证明什么（以及为什么必须证明这件事）

v0.10.10 及以前：给虚拟机配 4GB，系统只认出 2GB。**两条独立缺陷叠在一起**：

1. `kernel/mem64.cpp` 的页位图只有 64 KiB（`MEM64_BITMAP_BYTES`），只能记
   64K*8 = 524,288 页 = **2 GiB**，于是 `e820_mem_high64()` 里
   `if (highest > MEM64_POOL_MAX) highest = MEM64_POOL_MAX;` 把 E820 上界夹到 2.16 GiB。
   实测（-m 4096）：`[LM64] E820 usable_above_1M_kb=4193152`（4.0 GiB）却
   `[MEM64] pool=[0x0A000000,0x8A000000) pages=524288`（2.0 GiB）。
2. 页池被当成**一整段连续 RAM**（`g_page_total = (pool_end - pool_base) / PAGE`）。
   4GB 机器的 E820 是 `[1MiB,3.0GiB) usable` + `[3.0GiB,4GiB) PCI 空洞` + `[4GiB,5GiB) usable`，
   旧代码只是"恰好"没跨到空洞才没出事；一旦放宽上限，空洞会被当成可用 RAM 分配出去
   —— 那不是"分配失败"，那是往设备地址空间里写数据。

所以本测试的判据不是"数变大了"，而是**三条互相独立、互相不能冒充的证据**：
  ① 总内存口径：`mem_total_ram_64()` 必须与 E820 usable 求和逐 MiB 一致（4GB 机器 > 3.8 GiB）；
  ② 高地址真实可用：页池**最高地址**的页被分配出来、写入、读回一致，且地址 > 2 GiB
     （判据取 > 0x90000000 = 2.25 GiB，比交接单的 2 GiB 更严）—— 只看"分配成功"是骗人的；
  ③ PCI 空洞没被当成 RAM：4096 下"池跨度页数 - 可用页数"必须≈1 GiB/4KiB，
     512MB 下必须**完全相等**（那种机器没有空洞）。

## 判据加强记录（相对交接单 §3.1 的验收判据）

* 交接单②写"分配并读写一个 phys > 0x90000000 的页"→ 本测试要求的是**池顶页**
  （地址最高的可用页），比"随便一个高地址页"更强：池顶页正确说明位图末尾也正确。
* 交接单①写 `mem_total_ram_64() > 3.8 GiB` → 本测试额外要求它与 `[LM64] E820 usable_above_1M_kb`
  逐 MiB 对得上（±1 MiB），防止"把常量写大"这种假绿。
* 额外加 ③：空洞页必须真的不进页池（旧缺陷 2 的回归锁）。
* 额外加：`-m 512` 的 pool/pages 必须与 v0.10.10 实测值（`pages=90080`）**逐数字相同**，
  锁住"修 4GB 不许顺手改动小内存机器行为"。

用法：py -3 -u tests/mem4g64_test.py
"""
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

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYS_IMG = os.path.join(ROOT, "build64", "system.img")
BASE_IMG = os.path.join(SCRATCH, "BASE_system.img")

LOCK_INTERACTIVE = "[LOCK64] bg blur ready"

# v0.10.10 实测基线（-m 512，改动前）：pool=[0x0A000000,0x1FFE0000) pages=90080
SMALL_POOL_PAGES = 90080
SMALL_POOL_END = 0x1FFE0000

fails = []
checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    if not ok:
        fails.append(name)
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, (" —— " + detail) if detail else ""))


def find_qemu():
    for c in QEMU_CANDIDATES:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            from shutil import which
            p = which(c)
            if p:
                return p
    return None


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Vm:
    def __init__(self, qemu, img, mb, tag):
        self.qemu, self.img, self.mb, self.tag = qemu, img, mb, tag
        self.tmp = tempfile.mkdtemp(prefix="mem4g_%s_" % tag, dir=SCRATCH)
        self.disk = os.path.join(self.tmp, "d.img")
        self.serial = os.path.join(self.tmp, "ser.log")
        self.proc = None
        shutil.copyfile(img, self.disk)

    def start(self):
        self.proc = subprocess.Popen([
            self.qemu, "-name", "Vimtu64-mem" + self.tag,
            "-drive", "format=raw,file=%s" % self.disk,
            "-boot", "order=c", "-m", str(self.mb), "-vga", "std",
            "-display", "none", "-serial", "file:%s" % self.serial,
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % free_port(),
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return needle in self.log()

    def stop(self):
        try:
            self.proc.kill()
            self.proc.wait(timeout=10)
        except Exception:
            pass


def parse(log):
    """把本次启动的内存相关打点全部解析出来。任何一项缺失都返回 None（= 断言失败）。"""
    d = {}
    m = re.search(r"\[LM64\] E820 usable_above_1M_kb=(\d+)", log)
    d["e820_kb"] = int(m.group(1)) if m else None
    m = re.search(r"\[MEM64\] layout bss_end=([0-9a-fA-F]+) .*? pool=\[([0-9a-fA-F]+),([0-9a-fA-F]+)\) (guard=\w+)", log)
    if m:
        d["bss_end"] = int(m.group(1), 16)
        d["layout_pool"] = (int(m.group(2), 16), int(m.group(3), 16))
        d["guard"] = m.group(4)
    else:
        d["guard"] = None
    m = re.search(r"\[MEM64\] pool=\[([0-9a-fA-F]+),([0-9a-fA-F]+)\) pages=(\d+)/(\d+) free=(\d+)", log)
    if m:
        d["pool_base"] = int(m.group(1), 16)
        d["pool_end"] = int(m.group(2), 16)
        d["pages"] = int(m.group(3))
        d["span"] = int(m.group(4))
        d["free"] = int(m.group(5))
    m = re.search(r"\[MEM64\] ram total=(\d+)MiB.*?above4g=(\d+)MiB pool=(\d+)MiB direct_limit=(\d+)MiB", log)
    if m:
        d["ram_mib"] = int(m.group(1))
        d["above4g_mib"] = int(m.group(2))
        d["pool_mib"] = int(m.group(3))
        d["direct_mib"] = int(m.group(4))
    m = re.search(r"\[MEM64\] selftest high_page pa=0x([0-9a-fA-F]+) rw=(\d) gt2g=(\d) req=(\d)", log)
    if m:
        d["high_pa"] = int(m.group(1), 16)
        d["high_rw"] = int(m.group(2))
        d["high_gt2g"] = int(m.group(3))
        d["high_req"] = int(m.group(4))
    m = re.search(r"\[MEM64\] selftest (PASS|FAIL) mask=(\d+)", log)
    if m:
        d["selftest"] = m.group(1)
        d["selftest_mask"] = int(m.group(2))
    return d


def run_case(qemu, mb, tag, timeout=240):
    print("\n===== -m %d（%s）=====" % (mb, tag))
    vm = Vm(qemu, BASE_IMG if os.path.exists(BASE_IMG) else SYS_IMG, mb, tag)
    vm.start()
    try:
        ok = vm.wait_log(LOCK_INTERACTIVE, timeout)
        log = vm.log()
    finally:
        vm.stop()

    if not ok:
        check("[%d] 启动到可交互锁屏（%s）" % (mb, LOCK_INTERACTIVE), False,
              "串口 %d 字节；末尾: %r" % (len(log), log[-400:]))
        return None, log

    d = parse(log)
    # ---- 基线（与内存大小无关、任何机器都必须成立） ----
    check("[%d] 运行期四段布局断言 guard=ok" % mb, d.get("guard") == "guard=ok", str(d.get("guard")))
    check("[%d] [MEM64] selftest PASS（mask=0）" % mb,
          d.get("selftest") == "PASS" and d.get("selftest_mask") == 0,
          "%s mask=%s" % (d.get("selftest"), d.get("selftest_mask")))
    check("[%d] 无 PANIC / BSOD" % mb, "PANIC" not in log and "BSOD" not in log)
    check("[%d] 池顶页读写往返一致（rw=1）" % mb, d.get("high_rw") == 1, str(d.get("high_rw")))
    check("[%d] 池顶页地址 > 2 GiB（gt2g=1，硬要求见 req）" % mb,
          d.get("high_gt2g") == 1 or d.get("high_req") == 0,
          "pa=0x%X gt2g=%s req=%s" % (d.get("high_pa") or 0, d.get("high_gt2g"), d.get("high_req")))
    # 总内存口径 = E820 usable 求和（逐 MiB 对得上，±1）
    if d.get("e820_kb") and d.get("ram_mib"):
        want = d["e820_kb"] / 1024.0
        check("[%d] ram total 与 [LM64] E820 usable 逐 MiB 对账" % mb,
              abs(want - d["ram_mib"]) <= 1.0,
              "E820=%.1f MiB / ram total=%d MiB" % (want, d["ram_mib"]))
    else:
        check("[%d] ram total 与 [LM64] E820 usable 逐 MiB 对账" % mb, False,
              "e820_kb=%s ram_mib=%s" % (d.get("e820_kb"), d.get("ram_mib")))
    check("[%d] pool 口径自洽：pages*4KiB == pool MiB（±1 MiB）" % mb,
          d.get("pages") is not None and d.get("pool_mib") is not None and
          abs(d["pages"] * 4096 / 1048576.0 - d["pool_mib"]) <= 1.0,
          "pages=%s pool_mib=%s" % (d.get("pages"), d.get("pool_mib")))
    return d, log


def case_4g(d):
    print("\n--- 4GB 专属判据 ---")
    check("① 4GB 机器报告的内存 > 3.8 GiB（旧版 ≈2.16 GiB）",
          (d.get("ram_mib") or 0) > 3891,
          "ram total=%s MiB" % d.get("ram_mib"))
    check("①b 直映上限口径 = 4096 MiB（页池不许越过 4GB）",
          d.get("direct_mib") == 4096, str(d.get("direct_mib")))
    # ② 池顶页必须真的在 2.25 GiB 以上（比交接单的 2 GiB 更严）
    check("② 池顶页物理地址 > 0x90000000（2.25 GiB）且读写通过",
          (d.get("high_pa") or 0) > 0x90000000 and d.get("high_rw") == 1,
          "pa=0x%X" % (d.get("high_pa") or 0))
    check("②b 大机器上 req=1（自检确实启用了高地址硬判据）",
          d.get("high_req") == 1, str(d.get("high_req")))
    # ②c 页池必须真的越过 2 GiB 那条旧天花板
    check("②c 页池上界 > 2 GiB（旧的 0x8A000000 天花板已消失）",
          (d.get("pool_end") or 0) > 0x80000000,
          "pool_end=0x%X" % (d.get("pool_end") or 0))
    # ③ PCI 空洞不许进页池：跨度 - 可用 必须**精确**等于空洞页数。
    #    4GB 机器的空洞 = 0xBFFE0000..0x100000000
    #                   = 1 GiB（PCI MMIO 0xC0000000..0x100000000）
    #                   + 128 KiB（E820[4] base=0xBFFE0000 len=0x20000 type=2 reserved）
    #                   = 262144 + 32 = **262176 页**
    #    （第一版写成 262144±8，实测 262176 —— 是判据算错，不是产品错，已改精确值。）
    if d.get("pages") and d.get("span"):
        hole = d["span"] - d["pages"]
        check("③ 3GB..4GB PCI 空洞没被当成 RAM（跨度-可用 = 262176 页精确值）",
              hole == 262176,
              "span=%d pages=%d hole=%d" % (d["span"], d["pages"], hole))
    else:
        check("③ 3GB..4GB PCI 空洞没被当成 RAM（跨度-可用 = 262176 页精确值）", False,
              "span/pages 未解析")
    check("④ 4GB 以上 remap 段如实报出 ≈1024 MiB（可用但无直映）",
          abs((d.get("above4g_mib") or 0) - 1024) <= 2,
          "above4g=%s MiB" % d.get("above4g_mib"))
    # ⑤ pool 容量 = 可用字节和，且 >= 2.7 GiB（旧版 2.0 GiB）
    check("⑤ 页池容量 >= 2.7 GiB（旧版正好 2048 MiB）",
          (d.get("pool_mib") or 0) >= 2765,
          "pool=%s MiB" % d.get("pool_mib"))


def case_small(d):
    print("\n--- 512MB 机器：行为不许变 ---")
    check("③ 512MB：可用页数 == v0.10.10 基线 90080（逐数字相同）",
          d.get("pages") == SMALL_POOL_PAGES, str(d.get("pages")))
    check("③b 512MB：池上界 == 0x1FFE0000（E820 上界，未被新逻辑改动）",
          d.get("pool_end") == SMALL_POOL_END,
          "0x%X" % (d.get("pool_end") or 0))
    check("③c 512MB：无空洞 -> 跨度 == 可用页数",
          d.get("span") == d.get("pages"),
          "span=%s pages=%s" % (d.get("span"), d.get("pages")))
    check("③d 512MB：ram total 与 E820 对得上（约 510 MiB，不许报成 4GB）",
          500 <= (d.get("ram_mib") or 0) <= 512, "ram total=%s MiB" % d.get("ram_mib"))
    check("③e 512MB：above4g=0（没有 4GB 以上内存）",
          d.get("above4g_mib") == 0, str(d.get("above4g_mib")))
    check("③f 512MB：req=0（高地址硬判据只对大机器生效）",
          d.get("high_req") == 0, str(d.get("high_req")))


def main():
    qemu = find_qemu()
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(BASE_IMG) and not os.path.exists(SYS_IMG):
        sys.stderr.write("找不到 system.img（build64/ 或 BASE_system.img）\n")
        return 2

    d1, _ = run_case(qemu, 4096, "4096")
    if d1:
        case_4g(d1)
    d2, _ = run_case(qemu, 512, "512")
    if d2:
        case_small(d2)

    total = len(checks)
    bad = len(fails)
    print("\n================ 结果：%d/%d 通过 ================" % (total - bad, total))
    for n in fails:
        print("  FAILED: %s" % n)
    print("===== mem4g64_test: %s =====" % ("PASS" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
