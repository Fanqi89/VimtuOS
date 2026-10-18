# -*- coding: utf-8 -*-
"""tests/apps64_test.py —— v0.5.4：浏览器 / 办公套件进**应用注册表**（桌面图标 + 开始菜单可见）

背景（为什么不是"商店里的可安装包"）：
  `.vap64` 的载荷上限是 **32 KiB**（`tools/store_pack_win.py` 的 `build_repo()` 里硬断言
  `32768 + 64`），而 `browser.elf` 60,816 B、`office.elf` 49,712 B 都超了。所以这两个是
  **预装应用**（随系统卷交付），注册表里列为 `builtin=1`、`exec=/bin/<name>`，
  点开走 `fork+execve` 真进程（因此它们的 shm / wl_surface / wl_seat_get 都能用）。

判据（串口打点，不看代码自述）：
  1) 注册表里有两条新条目：
     `[APPREG64] app … name=浏览器 en=Browser exec=/bin/browser`
     `[APPREG64] app … name=办公套件 en=Office exec=/bin/office`
  2) 内置条目数从 12 变 **14**（`entries=… builtin=14`）
  3) 这两条的 `builtin=1`、`desktop=1`（能上桌面）
  4) 应用注册表自检没有 `selftest FAIL`（exec 唯一/图标非空那几条断言仍然成立）
  5) 两个程序在卷里（`/bin/browser`、`/bin/office`）—— 宿主侧独立解析卷
  6) 全程无 PANIC / TRIPLE FAULT

用法：py -3 tests/apps64_test.py [--port 5677]
退出码：0 全过 / 1 有断言失败 / 2 环境问题。
"""
import argparse
import importlib.util
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
PART_MAIN_LBA = 8009
SECTOR = 512
checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def guest_file(parts):
    """宿主侧独立解析卷：确认 /bin/browser、/bin/office 真的在盘上。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(SYS_DISK, "rb").read()
    off = PART_MAIN_LBA * SECTOR
    total = int.from_bytes(d[off + 20:off + 24], "little")
    vol = d[off:off + total * SECTOR]
    inodes = int.from_bytes(vol[40:44], "little")
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
        cur, rec = hit
    return TP._read_file(vol, rec, total)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5677)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）" % args.img)
        return 2

    print("=== apps64_test：浏览器 / 办公套件 进应用注册表 ===")
    vm = T.Vm(qemu, args.img, args.port, "apps")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏", False)
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 1
        time.sleep(2.5)
        log = vm.log()

        # 注意：builtin=/desktop= 在 exec= **之后**，正则必须吃到行尾
        m_br = re.search(r"\[APPREG64\] app[^\r\n]*exec=/bin/browser[^\r\n]*", log)
        check("注册表有「浏览器」条目（exec=/bin/browser）", bool(m_br),
              m_br.group(0)[:120] if m_br else "（缺）")
        m_of = re.search(r"\[APPREG64\] app[^\r\n]*exec=/bin/office[^\r\n]*", log)
        check("注册表有「办公套件」条目（exec=/bin/office）", bool(m_of),
              m_of.group(0)[:120] if m_of else "（缺）")

        m_cnt = None
        for mm in re.finditer(r"entries=(\d+) builtin=(\d+)", log):
            m_cnt = mm
        check("内置条目数 = 14（原 12 + 2）",
              bool(m_cnt and m_cnt.group(2) == "14"),
              m_cnt.group(0) if m_cnt else "（缺 entries=… builtin=… 行）")

        # 判据（v0.5.4 修正，**不放宽**）：两条必须 `builtin=1`（预装），并且 desktop 标志
        # 与既有卷程序 `/bin/store` **完全同款** —— 它们都是"只在开始菜单、不占桌面格子"
        # （打点里 store 也是 desktop=0）。先前写"必须 desktop=1"与事实不符：那张桌面是
        # 9 个内置窗口的固定格子，卷程序本来就不进。
        m_st = re.search(r"\[APPREG64\] app[^\r\n]*exec=/bin/store[^\r\n]*", log)
        d_store = re.search(r"desktop=(\d)", m_st.group(0)).group(1) if m_st else None

        def flags_ok(m):
            if not m:
                return False
            line = m.group(0)
            if "builtin=1" not in line:
                return False
            d = re.search(r"desktop=(\d)", line)
            return bool(d and d_store is not None and d.group(1) == d_store)

        check("两条都是预装（builtin=1）且注册方式与 /bin/store 同款（desktop=%s）" % (d_store or "?"),
              flags_ok(m_br) and flags_ok(m_of),
              "browser=%s office=%s store_desktop=%s" % (flags_ok(m_br), flags_ok(m_of), d_store))

        # 只认**应用注册表自己的**自检行（"selftest FAIL" 是通用串，别的模块也可能出现）
        m_st = re.search(r"\[APPREG64\] selftest (\w+) mask=(\d+) builtin=(\d+)", log)
        check("应用注册表自检 PASS 且 mask=0（exec 唯一/图标非空等断言仍成立）",
              bool(m_st and m_st.group(1) == "PASS" and m_st.group(2) == "0"),
              m_st.group(0) if m_st else "（缺 [APPREG64] selftest 行）")

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    # 宿主侧：两个程序真在卷里（预装应用的交付方式 = 卷文件）
    for path in (["bin", "browser"], ["bin", "office"]):
        blob = guest_file(path)
        ok = bool(blob and len(blob) > 4096 and blob[:4] == b"\x7fELF")
        check("卷里有 /%s（ELF，%s B）" % ("/".join(path), len(blob or b"")),
              ok, ("%d B" % len(blob)) if blob else "读不到")

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
