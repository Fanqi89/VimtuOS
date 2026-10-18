# -*- coding: utf-8 -*-
"""tests/persist64_test.py —— 任务B 验收：**同一块盘连续起两次 QEMU**，数据必须跨重启存活。

判据（每一条都能独立证伪）：
  1) 第一次启动：/bin/office 打字 -> F2（切模式时自动存 /tmp/doc.vwd）-> `[OFFICE] save mode=doc … rc>=0`
  2) 关掉 QEMU 之后，**宿主侧独立解析这块盘**：/tmp/doc.vwd 存在且含第一次的标记串
  3) 第二次启动（同一块盘）：`[OFFICE] seek mode=doc path=/tmp/doc.vwd rc=0 bytes=N2`，且 N2 == 第一次的 N1
     —— 读到的字节数与上次写下去的完全一致，说明 inode/数据块/CRC 都真写回了盘
  4) 第二次启动后继续打字 -> 保存 -> 宿主侧读卷：文件里**同时**含第一次与第二次的标记串
     （证明"读回的是上次内容"而不是"重新初始化成模板"）
  5) 全程无 PANIC / TRIPLE FAULT

用法：py -3 tests/persist64_test.py [--port 5677]
退出码：0 全过 / 1 有断言失败 / 2 环境问题（QEMU、镜像）。
"""
import argparse
import importlib.util
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
PART_MAIN_LBA = 8009
SECTOR = 512
WORK = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "persist")
os.makedirs(WORK, exist_ok=True)
IMG = os.path.join(WORK, "sys_p.img")

checks = []
MARK1 = "PERSIST1"
MARK2 = "PERSIST2"
OW, OH = 896, 580

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "=": "equal",
          "_": "shift-minus", ">": "shift-dot", "|": "shift-backslash",
          "'": "apostrophe", ":": "shift-semicolon",
          "+": "shift-equal", "*": "shift-8", "(": "shift-9", ")": "shift-0"}


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def guest_file(parts, img=None):
    """宿主侧独立解析卷（QEMU 停掉之后读）：证明文件真的落盘了。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img or IMG, "rb").read()
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


def key_for(ch):
    if ch in KEYMAP:
        return KEYMAP[ch]
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    return ch


def type_line(mon, s, per=0.10):
    for ch in s:
        mon.key(key_for(ch), wait=per)


def boot_to_office(vm, mon, tag):
    """锁屏 -> 桌面 -> 开始菜单 -> 终端 -> ring3 shell -> run /bin/office。"""
    if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
        check("%s: 等到锁屏" % tag, False)
        return False
    mon.key("ret", 1.4)
    mon.key("ret", 1.4)
    if not vm.wait_log(T.DESKTOP_READY, 180):
        check("%s: 进桌面" % tag, False)
        return False
    time.sleep(2.0)
    mon.key("meta_l", 1.2)
    mon.key("1", 2.0)
    if not vm.wait_log("[APP] term opened", 25):
        check("%s: 开场终端" % tag, False)
        return False
    time.sleep(1.2)
    n0 = len(vm.log())
    type_line(mon, "shell")
    mon.key("ret", wait=2.0)
    if not vm.wait_log("[SH64] launch", 60, since=n0):
        check("%s: 起 ring3 shell" % tag, False)
        return False
    time.sleep(2.5)
    n1 = len(vm.log())
    ok = False
    for _ in range(3):
        type_line(mon, "run /bin/office")
        mon.key("ret", wait=2.0)
        ok = vm.wait_log("[OFFICE] win", 45, since=n1)
        if ok:
            break
        print("  [info] %s: run 没被 shell 接住，重发" % tag)
        time.sleep(1.5)
    check("%s: 起 /bin/office（[OFFICE] win）" % tag, ok)
    if ok:
        vm.wait_log("[OFFICE] render", 30, since=n1)
        time.sleep(1.0)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SRC_DISK)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5677)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）" % args.img)
        return 2

    print("=== persist64_test：同一块盘连续起两次，验证数据跨重启存活 ===")
    shutil.copyfile(args.img, IMG)
    print("  [info] 工作盘副本 %s（%d B）" % (IMG, os.path.getsize(IMG)))

    # ================= 第一次启动：写 =================
    n_save1 = None
    vm = T.Vm(qemu, IMG, args.port, "p1")
    try:
        mon = vm.monitor()
        if not mon:
            check("第一次: 连上 monitor", False)
            return 1
        if not boot_to_office(vm, mon, "第一次"):
            return 2
        n2 = len(vm.log())
        type_line(mon, MARK1, per=0.12)
        time.sleep(1.5)
        check("第一次: 打字有打点", vm.wait_log("[OFFICE] key", 15, since=n2))
        n3 = len(vm.log())
        mon.key("f2", wait=2.5)                       # F2 切模式 -> 自动落盘 doc
        vm.wait_log("[OFFICE] save mode=doc", 25, since=n3)
        m_save = re.search(r"\[OFFICE\] save mode=doc path=(\S+) bytes=(\d+) rc=(-?\d+)",
                           vm.log()[n3:])
        ok_save = bool(m_save and int(m_save.group(3)) >= 0 and int(m_save.group(2)) > 0)
        check("第一次: 落盘 /tmp/doc.vwd（rc>=0 bytes>0）", ok_save,
              m_save.group(0) if m_save else "（缺）")
        if not ok_save:
            return 1
        n_save1 = int(m_save.group(2))
        n4 = len(vm.log())
        mon.key("esc", wait=2.0)
        check("第一次: ESC 退出 office", vm.wait_log("[OFFICE] done", 20, since=n4))
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("第一次: 无 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass
    time.sleep(1.0)

    # ---- 宿主侧独立解析：真的在盘上 ----
    blob = guest_file(["tmp", "doc.vwd"])
    ok_disk = bool(blob and MARK1.encode() in blob)
    check("关掉 QEMU 后宿主侧读卷：/tmp/doc.vwd 含 %s" % MARK1, ok_disk,
          ("%d B" % len(blob)) if ok_disk else ("读不到" if not blob else "%d B 不含标记" % len(blob)))

    # ================= 第二次启动：同一块盘读回 =================
    vm = T.Vm(qemu, IMG, args.port + 1, "p2")
    n_read = None
    try:
        mon = vm.monitor()
        if not mon:
            check("第二次: 连上 monitor", False)
            return 1
        if not boot_to_office(vm, mon, "第二次"):
            return 2
        m_seek = re.search(r"\[OFFICE\] seek mode=doc path=(\S+) rc=(-?\d+) bytes=(\d+)", vm.log())
        rc = int(m_seek.group(2)) if m_seek else None
        n_read = int(m_seek.group(3)) if m_seek else None
        check("第二次: 读回 /tmp/doc.vwd（rc=0 bytes>0）",
              bool(m_seek and rc == 0 and n_read > 0),
              m_seek.group(0) if m_seek else "（缺）")
        check("第二次: 读回字节数与上次写下去的一致（%s == %s）" % (n_read, n_save1),
              n_read == n_save1, "上次 save=%s，这次 seek=%s" % (n_save1, n_read))

        # 在读回的内容上继续写 -> 证明不是被重新初始化
        n5 = len(vm.log())
        type_line(mon, MARK2, per=0.12)
        time.sleep(1.5)
        n6 = len(vm.log())
        mon.key("f2", wait=2.5)
        vm.wait_log("[OFFICE] save mode=doc", 25, since=n6)
        m_save2 = re.search(r"\[OFFICE\] save mode=doc path=(\S+) bytes=(\d+) rc=(-?\d+)",
                            vm.log()[n6:])
        check("第二次: 再落盘（rc>=0）",
              bool(m_save2 and int(m_save2.group(3)) >= 0),
              m_save2.group(0) if m_save2 else "（缺）")
        n7 = len(vm.log())
        mon.key("esc", wait=2.0)
        vm.wait_log("[OFFICE] done", 20, since=n7)
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("第二次: 无 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass
    time.sleep(1.0)

    blob2 = guest_file(["tmp", "doc.vwd"])
    has1 = bool(blob2 and MARK1.encode() in blob2)
    has2 = bool(blob2 and MARK2.encode() in blob2)
    check("第二次之后宿主侧读卷：文件同时含 %s 与 %s（上次内容还在）" % (MARK1, MARK2),
          has1 and has2,
          ("%d B, %s=%s %s=%s" % (len(blob2), MARK1, has1, MARK2, has2)) if blob2 else "读不到")

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
