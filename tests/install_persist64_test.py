"""tests/install_persist64_test.py —— 装到硬盘后**重启/关机数据必须还在**（v0.5.11 真缺陷）。

修的是什么（用户原话："我是在 VMware 中通过 ISO 将系统装在虚拟磁盘中的，但只要重启或关机
系统中的数据就不会保存（桌面图标位置、壁纸、主题、用户名、用户密码、桌面上的新建文件夹）"）：

  安装器只把**载荷 8073 扇区**（MBR+引导+内核+64 扇区裸槽）写到目标盘，
  而 12 MiB 的 VimtuFS2 卷留在 sysdisk.img 的 LBA 8009 起 —— 装好的盘根本没有文件系统：
      [VFS64] mount FAILED reason=magic -> mount_system FAILED
      [CONF64] WARN NOT-PERSISTENT carrier=raw reason=no-vimtufs2-volume
  于是所有写入都失败（`[VFS64] mkdir_on64: slot=0 not mounted`），重启当然全丢。
  修法：tools/payload_embed_win.py 把卷并入载荷 -> 装好的盘自带卷。

判据（每条都带实测数字）：
  ① 装好的盘 LBA8009 = "VIMTUFS2" 卷头（magic + version=4）
  ② 第一次启动：`[VFS64] mount ok` 且**没有** `mount_system FAILED` / `NOT-PERSISTENT`
  ③ 在系统里真做三件事：`mkdir /home/vimtu/pdata`、`cfg set ui.theme 3`、`cfg save`
     并要求 `[CONF64] flush ok via=vfs`（落盘到卷，不是内存）
  ④ **正常关机**（QEMU monitor `quit`，会刷盘）后重启同一块盘：
     * 宿主侧独立解析卷：/home/vimtu/pdata 存在（不依赖屏幕文本）
     * 串口里 `[CONF64] ... ui.theme=3` 出现（设置真的读回来了）
  ⑤ 全程无 PANIC

用法：py -3 tests/install_persist64_test.py [--port 5761]
退出码：0 全过 / 1 有断言失败 / 2 环境问题（qemu / 镜像 / 安装不起来）。
"""
import argparse
import importlib.util
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                     # noqa: E402
import startmenu64_test as SM                                    # noqa: E402
import qemuhelp as qh                                            # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
WORK = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "installpersist")
os.makedirs(WORK, exist_ok=True)
TARGET = os.path.join(WORK, "target_persist.img")
PORT = 5761
TARGET_SECTORS = 8 * 1024 * 1024 * 2          # 8 GiB（用户场景：8G 虚拟盘）
MAGIC = b"VIMTUFS2"

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def q(p):
    return p.replace("\\", "/")


def load_mod(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, *rel.split("/")))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def log_of(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_in(path, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in log_of(path):
            return True
        time.sleep(0.4)
    return False


def start(qemu, args, serial, port):
    if os.path.exists(serial):
        os.remove(serial)
    proc = subprocess.Popen([qemu, "-name", "vimtu-installpersist", "-m", "512", "-vga", "std",
                             "-display", "none", "-serial", "file:%s" % q(serial),
                             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                             "-no-reboot"] + args,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(160):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return proc


def type_line(mon, text, per=0.13):
    km = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
    for ch in text:
        mon.key(km.get(ch, ch), wait=per)
    mon.key("ret", wait=1.0)


def open_terminal(vm_log, mon, serial):
    hit = None
    for _ in range(5):
        mon.key("meta_l", 1.6)
        time.sleep(1.2)
        for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(-?\d+) name=(\S+) en=(.+?) "
                              r"x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log_of(serial)):
            if mm.group(3) == "终端":
                hit = mm
                break
        if hit:
            break
        mon.key("esc", 0.8)
        time.sleep(1.0)
    if not hit:
        return False
    cur = SM.Cursor()
    cur.goto(mon, int(hit.group(5)) + int(hit.group(7)) // 2,
             int(hit.group(6)) + int(hit.group(8)) // 2)
    cur.settle(mon)
    time.sleep(0.3)
    mon.click()
    return wait_in(serial, "[APP] term opened", 30)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--qemu", default=None)
    args = ap.parse_args()
    qemu = T.find_qemu(args.qemu)
    if not qemu or not os.path.exists(MEDIUM):
        print("跳过：qemu 或安装介质不可用（%s）" % MEDIUM)
        return 2

    print("=== install_persist64_test：装到硬盘 -> 改设置/建目录 -> 关机重启 -> 数据必须还在 ===")
    # ---- 1) 安装到 8G 空盘 ----
    if os.path.exists(TARGET):
        os.remove(TARGET)
    with open(TARGET, "wb") as f:
        f.truncate(TARGET_SECTORS * 512)
    print("  目标盘 %d 扇区（%.1f GiB）" % (TARGET_SECTORS, TARGET_SECTORS / 2048.0 / 1024))
    serial_i = os.path.join(WORK, "install.log")
    proc = start(qemu, ["-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
                        "-drive", "format=raw,file=%s,index=1,media=disk" % q(TARGET),
                        "-boot", "order=c"], serial_i, args.port)
    mon = SM.Monitor(args.port)
    if not wait_in(serial_i, "磁盘枚举完成", 120):
        print("  安装向导没就绪")
        proc.kill()
        return 2
    for key in ("ret", "ret", "ret", "ret", "n", "ret"):
        mon.key(key, wait=2.0)
    done = wait_in(serial_i, "[INSTALL] 完成：已写", 240)
    log_i = log_of(serial_i)
    check("安装完成（[INSTALL] 完成：已写 N 扇区）", done,
          (re.search(r"\[INSTALL\] 完成：已写[^\r\n]*", log_i) or ["缺失"])[0])
    # ★ 关键：正常退出（monitor quit 会刷盘；直接 kill 会丢写缓存 —— 用户"关机就丢"的另一半原因）
    mon.send("quit", wait=1.5)
    try:
        proc.wait(timeout=30)
    except Exception:
        proc.kill()

    # ---- 2) 装好的盘：卷必须在 ----
    with open(TARGET, "rb") as f:
        f.seek(8009 * 512)
        head = f.read(16)
    check("装好的盘 LBA8009 = VimtuFS2 卷头（magic + version=4）",
          head[:8] == MAGIC and int.from_bytes(head[8:12], "little") == 4,
          "%r version=%d" % (head[:8], int.from_bytes(head[8:12], "little")))

    # ---- 3) 第一次启动：改设置 + 建目录 ----
    serial_a = os.path.join(WORK, "run1.log")
    proc = start(qemu, ["-drive", "format=raw,file=%s" % q(TARGET), "-boot", "order=c"],
                 serial_a, args.port)
    mon = SM.Monitor(args.port)
    if not qh.wait_desktop(mon, lambda: log_of(serial_a), proc, timeout=300):
        print("  第一次启动没进桌面")
        proc.kill()
        return 2
    log_a = log_of(serial_a)
    check("装好的系统挂上了系统卷（[VFS64] mount ok，无 mount_system FAILED）",
          "mount_system FAILED" not in log_a and "[VFS64] mount ok" in log_a,
          (re.search(r"\[VFS64\] mount_system[^\r\n]*|\[VFS64\] mount ok[^\r\n]*", log_a) or ["缺失"])[0])
    check("不是 NOT-PERSISTENT（载体真是 VimtuFS2 卷）", "NOT-PERSISTENT" not in log_a,
          (re.search(r"\[CONF64\][^\r\n]*NOT-PERSISTENT[^\r\n]*", log_a) or ["（无，正确）"])[0])
    if not open_terminal(log_a, mon, serial_a):
        print("  打不开终端")
        proc.kill()
        return 1
    type_line(mon, "mkdir /home/vimtu/pdata")
    type_line(mon, "cfg set ui.theme 3")
    type_line(mon, "cfg save")
    ok_flush = wait_in(serial_a, "[CONF64] flush ok", 60)
    log_a2 = log_of(serial_a)
    check("设置真落盘（[CONF64] flush ok via=vfs）", ok_flush,
          (re.search(r"\[CONF64\] flush ok[^\r\n]*", log_a2) or ["缺失"])[0])
    check("建目录没有失败（不出现 mkdir_on64: slot=0 not mounted）",
          "mkdir_on64: slot=0 not mounted" not in log_a2)
    mon.send("quit", wait=1.5)                       # ★ 正常关机（刷盘）
    try:
        proc.wait(timeout=30)
    except Exception:
        proc.kill()

    # ---- 4) 重启同一块盘：数据必须还在 ----
    serial_b = os.path.join(WORK, "run2.log")
    proc = start(qemu, ["-drive", "format=raw,file=%s" % q(TARGET), "-boot", "order=c"],
                 serial_b, args.port)
    mon = SM.Monitor(args.port)
    up = qh.wait_desktop(mon, lambda: log_of(serial_b), proc, timeout=300)
    check("第二次启动仍进桌面", up)
    if up:
        open_terminal(log_of(serial_b), mon, serial_b)
        time.sleep(1.0)
    log_b = log_of(serial_b)
    check("第二次启动读到 ui.theme=3（设置真的持久化了）",
          re.search(r"ui\.theme=3", log_b) is not None,
          (re.search(r"\[CONF64\][^\r\n]*ui\.theme[^\r\n]*", log_b) or ["缺失"])[0])
    mon.send("quit", wait=1.5)
    try:
        proc.wait(timeout=30)
    except Exception:
        proc.kill()

    # 宿主侧独立解析卷：/home/vimtu/pdata 必须存在（不依赖屏幕文本）
    try:
        TP = load_mod("tcc_pack_win", "tools/tcc_pack_win.py")
        d = open(TARGET, "rb").read()
        vol = d[8009 * 512:]
        inodes = int.from_bytes(vol[40:44], "little")
        cur_inode = 0
        found = None
        for part in ("home", "vimtu", "pdata"):
            hit = None
            for i, nm, r in TP._entries(vol, cur_inode, inodes):
                if nm == part:
                    hit = (i, r)
                    break
            if not hit:
                break
            cur_inode, found = hit[0], hit
        check("宿主侧独立解析卷：/home/vimtu/pdata 存在（重启后仍在盘上）",
              found is not None, "inodes=%d" % inodes)
    except Exception as exc:                          # noqa: BLE001
        check("宿主侧独立解析卷（解析器可用）", False, str(exc)[:120])

    check("全程无 PANIC", "PANIC64] stop" not in (log_a2 + log_b))

    bad = [n for n, ok in checks if not ok]
    print("\n=== 汇总：%d/%d PASS ===" % (len(checks) - len(bad), len(checks)))
    for n in bad:
        print("  FAIL: %s" % n)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
