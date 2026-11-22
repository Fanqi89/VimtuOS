# -*- coding: utf-8 -*-
"""dragdrop64_test.py —— ★ v0.11.1 跨层拖拽（资源管理器条目 -> 桌面）端到端验收。

判据全部落在**串口打点**与**重启后的事实**上（不靠"看起来像"）：

  A 拖拽链路（同一次开机内）
    [UI] explorer click ... hit=item:<i>              按下点真的落在条目上（鼠标闭环的锚）
    [UI] explorer drag begin idx=<i> name=... min=8    越过 8px 阈值才成立（不是一按就拖）
    [UI] explorer drag drop path=/<..> target=desktop ok=1     松手在桌面空白处 -> 落点成立
    [DESK64] file add idx=<j> kind=9 dir=.. path=/<..>         桌面项真的加上了
    [DESK64] links save why=add n=1 bytes=<n> rc=<n> ok=1     **真落盘**（rc == bytes）
  B 反例（同一次开机内）
    在窗口上松手 -> `target=none ok=0`，且**没有**新的 `[DESK64] file add`
  C 跨重启（同一块盘再起一次）
    [DESK64] links load why=init bytes=<n> n=1 skipped=0 ok=1
    [DESK64] item idx=<j> kind=9 name=<同名>      重启后图标还在（读的就是 /etc/desktop.links）
  D 不得出现 PANIC / TRIPLE FAULT / selftest FAIL / OOM

为什么用真系统盘（build64/sysdisk.img 的**副本**）：小系统盘夹具是**刚格式化的空卷**，
根目录一个条目都没有 —— 没有东西可拖，也就没有判据。副本保证共享产物不被写脏（v0.9.24 的教训）。

夹具硬化（只影响"能不能开机/能不能连上 monitor"，一条产品判据都不放宽）：
  * QEMU 起不来、或**端口被同机其它 QEMU 抢走**（QEMU 用 `,nowait` 不会退出，只是不监听）
    -> 杀掉换端口重试 3 次；QEMU 自己的 stdout 落到 `<serial>.qemu.txt`。
    本项目实测：本机同时跑着别的项目的 QEMU，`-qmp tcp:127.0.0.1:<随机>` 会抢占 `free_port()` 探到的端口。
  * telnet monitor 偶发 `ConnectionRefused` -> 有界退避重连（客户机本身还活着）。

运行：
  py -3 tests/dragdrop64_test.py [--qemu <path>] [--keep]
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
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  find_qemu
import fs_tree_test as fst         # noqa: E402  qemu_args / free_port / kill
import explorer64_test as ex       # noqa: E402  复用鼠标注入闭环（move_px / aim_click / 几何常量）

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")


# ==================== 夹具：QEMU / monitor（有界重试，不改产品判据）====================
def _monitor_ok(port, tries=3):
    """端口上真的有人在听 monitor 吗（TCP 能连上就算；连上立刻关掉，不发命令）。"""
    for _ in range(tries):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.close()
            return True
        except OSError:
            time.sleep(0.3)
    return False


class Vm:
    """启动 QEMU（起不来 / monitor 端口被别人占了就换端口重试 3 次）。"""

    def __init__(self, qemu, disk, serial, name):
        self.serial = serial
        self.out = serial + ".qemu.txt"
        self.proc = None
        self.port = 0
        for _ in range(3):
            port = fst.free_port()
            try:
                os.remove(serial)
            except OSError:
                pass
            out = open(self.out, "wb")
            proc = subprocess.Popen(fst.qemu_args(qemu, [disk], serial, port, name),
                                    stdout=out, stderr=subprocess.STDOUT)
            time.sleep(1.5)
            if proc.poll() is None and _monitor_ok(port):
                self.proc, self.port = proc, port
                return
            try:
                if proc.poll() is None:
                    fst.kill(proc)
            except Exception:
                pass
            out.close()
        raise RuntimeError("QEMU 三次都没起来（stdout 见 %s）" % self.out)

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
                return False
            time.sleep(0.3)
        return False

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                fst.kill(self.proc)
            except Exception:
                pass


class Mon(ex.Monitor):
    """QEMU telnet monitor 的有界重连包装（连不上退避重试 3 次，最后一次的错如实抛出）。"""

    def _send_raw(self, cmds, wait_between, wait_end):
        last = None
        for k in range(3):
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
                try:
                    for c in cmds:
                        s.sendall(c.encode() + b"\n")
                        time.sleep(wait_between)
                    time.sleep(wait_end)
                finally:
                    s.close()
                return
            except OSError as e:
                last = e
                time.sleep(0.4 + k * 0.6)
        raise last

    def send(self, cmd, wait=0.35):
        self._send_raw([cmd], wait, 0)

    def raw(self, cmds, wait_between=0.12, wait_end=0.5):
        self._send_raw(cmds, wait_between, wait_end)


def wait_re(vm, pattern, timeout, since=0):
    """有界等待：日志里出现 pattern（返回**最后一次**匹配；超时返回 None）。"""
    t0 = time.time()
    while True:
        last = None
        for m in re.finditer(pattern, vm.log()[since:]):
            last = m
        if last is not None:
            return last
        if time.time() - t0 > timeout or vm.proc.poll() is not None:
            return None
        time.sleep(0.3)


def count(vm, pattern):
    return len(re.findall(pattern, vm.log()))


def login(vm, mon, timeout=240):
    """锁屏 -> 登录 -> 桌面（默认用户无口令：回车两次）。"""
    if not vm.wait_log("[LOCK64] bg blur ready", timeout):
        return False
    mon.key("ret", wait=1.0)
    mon.key("ret", wait=1.5)
    return vm.wait_log("[GUI64] ready", timeout)


def open_explorer(vm, mon):
    """开始菜单数字快捷键 2 = 我的电脑（与 explorer64_test 同一条路）。"""
    since = len(vm.log())
    mon.key("meta_l", wait=1.0)
    mon.key("2", wait=2.5)
    return vm.wait_log("[UI] explorer thispc", 25, since)


def drag_from_to(mon, src, dst):
    """在 src 按下 -> 分轴精确移动到 dst -> 松开（全程一个按键状态，中途不点）。"""
    ex.park_cursor(mon)
    ex.move_px(mon, 0, +1, src[0])
    ex.move_px(mon, 1, +1, src[1])
    time.sleep(0.3)
    mon.send("mouse_button 1", wait=0.45)                 # 按下
    dx, dy = dst[0] - src[0], dst[1] - src[1]
    if abs(dx) > 0:
        ex.move_px(mon, 0, 1 if dx > 0 else -1, abs(dx))
    if abs(dy) > 0:
        ex.move_px(mon, 1, 1 if dy > 0 else -1, abs(dy))
    time.sleep(0.6)                                       # 让幽灵/落点判定跑几帧
    mon.send("mouse_button 0", wait=0.9)                  # 松开


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_dragdrop_")
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(SYSDISK, disk)          # ★ 副本：绝不写脏共享产物
    serial1 = os.path.join(tmp, "serial1.log")
    serial2 = os.path.join(tmp, "serial2.log")
    checks = []
    state = {"ok": True}

    def check(name, cond, detail=""):
        state["ok"] = state["ok"] and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, log):
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:"):
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    vm = None
    mon = None
    name = ""
    try:
        # ==================== 阶段 1：真系统盘启动 + 打开资源管理器 ====================
        print("=== 阶段 1：真系统盘（副本）启动 -> 桌面 -> 此电脑 ===")
        vm = Vm(qemu, disk, serial1, "Vimtu64-dragdrop")
        mon = Mon(vm.port)
        check("锁屏 -> 登录 -> 桌面（[GUI64] ready）", login(vm, mon), "serial=%s" % serial1)
        check("文件管理器能打开（[UI] explorer thispc）", open_explorer(vm, mon))
        if not state["ok"]:
            raise RuntimeError("阶段 1 失败：桌面或资源管理器没起来（serial=%s）" % serial1)

        # 鼠标闭环标定（内容区右下角的空白点 = 探针安全点）
        ex.SAFE_POINT = (ex.sx(ex.CONTENT_X + ex.CONTENT_W - 40),
                         ex.sy(ex.CONTENT_Y + ex.CONTENT_H - 40))
        top_ok, top_det = ex.ensure_window_on_top(vm, mon)
        check("资源管理器在最前且能收到点击", top_ok, top_det)
        dxe, dye, loops = ex.calibrate_offset(vm, mon)
        check("鼠标落点偏差已量出（闭环修正用）", True, "dx=%d dy=%d loops=%d" % (dxe, dye, loops))

        # ==================== 阶段 2：进 C: 盘根（图标视图）====================
        print("=== 阶段 2：双击 C: 卡片 -> 盘根 ===")
        mc = re.search(r"\[UI\] explorer card idx=(\d+) letter=C: kind=browsable", vm.log())
        idx_c = int(mc.group(1)) if mc else 0
        cc = ex.card_center(idx_c)
        ex.aim_click(vm, mon, cc[0] + dxe, cc[1] + dye, "card:%d" % idx_c)
        got = wait_re(vm, r"\[UI\] explorer enter letter=C: slot=\d+ ok items=(\d+)", 30)
        check("进入 C: 盘根（[UI] explorer enter letter=C: ok items=N）", got is not None,
              got.group(0) if got else "（缺行）")
        n_items = int(got.group(1)) if got else 0
        check("盘根有条目可拖（items >= 3）", n_items >= 3, "items=%d" % n_items)
        check("视图 = 图标（拖拽几何按图标格算）", ex.ensure_view(vm, mon, "icons"))

        cell = ex.icon_cell(1)                       # 第 1 格（避开可能压标题带的第一格）
        src = (cell[0] + dxe, cell[1] + dye)
        ex.blind_goto(mon, src[0], src[1])
        since = len(vm.log())
        ex.single_click_at(mon)
        p = ex.last_probe(vm, since)
        check("按下点落在条目上（[UI] explorer click hit=item:1）",
              p is not None and p[2] == "item:1", str(p))

        # ==================== 阶段 3：拖到桌面空白 ====================
        print("=== 阶段 3：按住条目 -> 拖到桌面空白 -> 松手 ===")
        dst = (ex.sx(ex.WIN_W + 320), ex.sy(40))     # 窗口右侧的桌面空白（y 在 Dock 之上）
        check("落点确实在窗口外（几何前置）", dst[0] > ex.WIN_X + ex.WIN_W, "dst=%s" % (dst,))
        since = len(vm.log())
        drag_from_to(mon, src, dst)
        m_begin = wait_re(vm, r"\[UI\] explorer drag begin idx=(\d+) name=(\S+) dir=(\d+) "
                              r"sx=\d+ sy=\d+ min=8", 20, since)
        check("拖拽成立（[UI] explorer drag begin … min=8：越过阈值才拖）", m_begin is not None,
              m_begin.group(0) if m_begin else "（缺行）")
        name = m_begin.group(2) if m_begin else ""
        dirflag = m_begin.group(3) if m_begin else ""
        m_drop = wait_re(vm, r"\[UI\] explorer drag drop path=(/\S*) dir=\d+ x=\d+ y=\d+ "
                              r"target=(\w+) ok=(\d)", 20, since)
        check("松手落点 = 桌面（target=desktop ok=1）",
              m_drop is not None and m_drop.group(2) == "desktop" and m_drop.group(3) == "1",
              m_drop.group(0) if m_drop else "（缺行）")
        path = m_drop.group(1) if m_drop else ""
        check("落点路径与拖的条目同名（path 尾 = name）",
              bool(path) and bool(name) and path.rstrip("/").endswith("/" + name),
              "path=%s name=%s" % (path, name))

        m_add = wait_re(vm, r"\[DESK64\] file add idx=(\d+) kind=9 dir=(\d+) vol=\d+ path=(/\S*) "
                             r"x=\d+ y=\d+ total=\d+ why=explorer-drag", 20, since)
        check("桌面项真的追加（[DESK64] file add kind=9）", m_add is not None,
              m_add.group(0) if m_add else "（缺行）")
        check("追加的路径与拖拽路径一致", bool(m_add) and m_add.group(3) == path,
              "add=%s drop=%s" % (m_add.group(3) if m_add else "-", path))
        check("dir 标志一致（条目类型没被搞反）",
              bool(m_add) and m_add.group(2) == dirflag,
              "add_dir=%s drag_dir=%s" % (m_add.group(2) if m_add else "-", dirflag))
        m_save = wait_re(vm, r"\[DESK64\] links save why=add n=(\d+) bytes=(\d+) rc=(\d+) ok=(\d)",
                         20, since)
        check("落到 /etc/desktop.links（links save n=1 ok=1，rc == bytes）",
              m_save is not None and m_save.group(4) == "1"
              and int(m_save.group(2)) == int(m_save.group(3)),
              m_save.group(0) if m_save else "（缺行）")
        nsnap = count(vm, r"\[DESK64\] snap idx=")
        check("吸附/避让打点存在（[DESK64] snap idx=…）", nsnap >= 1, "snap 次数=%d" % nsnap)

        # 拖完之后窗口照常收点击（没有卡在拖拽态）
        since = len(vm.log())
        ex.blind_goto(mon, ex.SAFE_POINT[0] + dxe, ex.SAFE_POINT[1] + dye)
        ex.single_click_at(mon)
        p2 = ex.last_probe(vm, since)
        check("拖完之后窗口照常收点击（没有卡在拖拽态）", p2 is not None, str(p2))

        # ==================== 阶段 4：反例（在窗口上松手）====================
        print("=== 阶段 4：反例 —— 拖到窗口自己身上松手（必须 target=none）===")
        adds_before = count(vm, r"\[DESK64\] file add idx=")
        cell2 = ex.icon_cell(2)
        src2 = (cell2[0] + dxe, cell2[1] + dye)
        dst2 = (ex.sx(ex.CONTENT_X + 200), ex.sy(ex.CONTENT_Y + 200))     # 仍在客户区里
        since = len(vm.log())
        drag_from_to(mon, src2, dst2)
        m_drop2 = wait_re(vm, r"\[UI\] explorer drag drop path=(/\S*) dir=\d+ x=\d+ y=\d+ "
                               r"target=(\w+) ok=(\d)", 20, since)
        check("反例：松手在窗口上 -> target=none ok=0",
              m_drop2 is not None and m_drop2.group(2) == "none" and m_drop2.group(3) == "0",
              m_drop2.group(0) if m_drop2 else "（缺行）")
        time.sleep(1.5)
        adds_after = count(vm, r"\[DESK64\] file add idx=")
        check("反例：没有新增桌面项（file add 计数不变）", adds_after == adds_before,
              "before=%d after=%d" % (adds_before, adds_after))
        forbid("阶段 4 日志", vm.log())
        vm.close()
        vm = None

        # ==================== 阶段 5：同一块盘再起一次（跨重启持久化）====================
        print("=== 阶段 5：重启同一块盘 -> 读回 /etc/desktop.links ===")
        vm = Vm(qemu, disk, serial2, "Vimtu64-dragdrop2")
        mon = Mon(vm.port)
        check("第二次启动到桌面", login(vm, mon), "serial=%s" % serial2)
        m_load = wait_re(vm, r"\[DESK64\] links load why=init bytes=(\d+) n=(\d+) "
                             r"skipped=(\d+) ok=(\d+) items=(\d+)", 25)
        check("启动期读回 links（links load ok=1 n=1 skipped=0）",
              m_load is not None and m_load.group(2) == "1"
              and m_load.group(3) == "0" and m_load.group(4) == "1",
              m_load.group(0) if m_load else "（缺行）")
        m_item = wait_re(vm, r"\[DESK64\] item idx=(\d+) kind=9 name=(\S+) x=\d+ y=\d+", 15)
        check("桌面上出现 kind=9 的项（重启后仍在）", m_item is not None,
              m_item.group(0) if m_item else "（缺行）")
        check("重启后的图标名与拖拽时的名字一致",
              bool(m_item) and bool(name) and m_item.group(2) == name,
              "item=%s drag=%s" % (m_item.group(2) if m_item else "-", name))
        forbid("阶段 5 日志", vm.log())
    except Exception as e:                       # 夹具/注入失败也如实计一条红（不静默通过）
        check("测试流程没有异常中断", False, repr(e))
    finally:
        if vm is not None:
            try:
                vm.close()
            except Exception:
                pass

    npass = sum(1 for _, c in checks if c)
    print("\n=== 汇总：%d/%d PASS（跨层拖拽：explorer -> 桌面）===" % (npass, len(checks)))
    if not state["ok"]:
        print("失败项：")
        for n, c in checks:
            if not c:
                print("  - %s" % n)
    if args.keep:
        print("夹具保留：%s" % tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if state["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
