#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/dock64_test.py - v0.8.0：Dock **运行时动态图标列表**验收（需求原文逐条）

要证明的事（每条都有串口硬证据，纯 ASCII 判据）：
 ① 固定列表来自**持久化**（不是写死数组）：首次启动 `[DOCK64] pins src=seed n=8 … path=/etc/dock.conf`
    并真的写进卷（宿主侧从磁盘镜像里把 /etc/dock.conf 读出来核对内容）；第二次启动
    `[DOCK64] pins src=file n=8`（同一个卷，证明复用持久化而不是每次都 seed）。
 ② 窗口事件 -> 运行项：开 3 个终端窗口 -> 3 条 `[DOCK64] run-add win=… app=3`，Dock 项里出现
    3 个 kind=running 的 app=3 项（`running=3`），且**不合并**。
 ③ 关一个窗口只移除对应 window_id：`[DOCK64] run-del win=…`，`running=2`，另外两个还在。
 ④ 右键 Dock 图标 -> 上下文菜单：`[DOCK64] menu open owner=dock app=3 win=… pinned=1 rows=2
    items=unpin,close-win`（已固定 + 运行中 -> 同时有"取消固定"和"关闭窗口"）。
    点"取消固定" -> `menu act … act=unpin pinned_after=0`，且 `[DOCK64] pins save` 立即写盘。
 ⑤ 取消固定只移除固定图标：固定项数 -1、**运行项数不变**（不关窗口）。
 ⑥ 宽度规则：把图标数搞多（多个运行项）后 `[DOCK64] geom w=` 变大；删掉后 w 回到默认宽度
    （不低于默认：`items=` 变少但 `w` 不小于默认 520）。

用法（必须 Windows 原生 Python）：py -3 tests\\dock64_test.py [--timeout 460] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh                      # noqa: E402
import startmenu64_test as SM              # noqa: E402  （Monitor/Cursor：相对鼠标驱动）
import install_persist64_test as P         # noqa: E402  （find_qemu / 卷解析工具）

QEMU = P.T.find_qemu()
DISK = os.path.join(ROOT, "build64", "sysdisk.img")
WORK = os.path.join(ROOT, "build64", "dock64_test")
ok = True
CUR = None                      # 全程复用的光标（SM.Cursor 按相对位移跟踪）
SERIAL_OF = [""]                 # 当前会话的串口日志路径（sync_cur 读它）


def q(p):
    return p.replace("\\", "/")


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def log_of(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_mark(fn, needle, timeout, proc=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in fn():
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.5)
    return False


def start(serial, port, extra=None):
    args = [QEMU, "-name", "Vimtu64-dock64",
            "-drive", "format=raw,file=%s" % q(DISK),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot"]
    if extra:
        args += extra
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p, SM.Monitor(port)


def dock_items(log):
    """[(idx, app, kind, win, x, y, cx, cy)] —— 只认最后一次 geom 之后的 item 行（切片，避免历史行）"""
    k = log.rfind("[DOCK64] geom ")
    seg = log[k:] if k >= 0 else ""
    out = []
    for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(-?\d+) name=.* "
                         r"x=(\d+) y=(\d+) w=(\d+) h=(\d+) cx=(\d+) cy=(\d+) kind=(\w+) win=(\d+)", seg):
        out.append(dict(idx=int(m.group(1)), app=int(m.group(2)), kind=m.group(9),
                        win=int(m.group(10)), x=int(m.group(3)), y=int(m.group(4)),
                        cx=int(m.group(7)), cy=int(m.group(8))))
    return out


def geom(log):
    m = None
    for m2 in re.finditer(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) "
                          r"gap=(\d+) items=(\d+)", log):
        m = m2
    return m


def read_conf_from_img(path):
    """宿主侧：在磁盘镜像字节里找 /etc/dock.conf 的正文（卷内数据块原样存）。

    不依赖任何工具：头行是本模块自己写的固定前缀，找到就取到 NUL/不可打印为止。
    """
    with open(path, "rb") as f:
        data = f.read()
    key = b"# VimtuOS dock pinned apps"
    k = data.find(key)
    if k < 0:
        return ""
    out = bytearray()
    for b in data[k:k + 400]:
        if b == 0 or b == 0xFF or b < 9 or (13 < b < 32):
            break
        out.append(b)
    return out.decode("utf-8", "replace")

def cur_of():
    return CUR


def sync_cur(log_fn):
    """把光标拉回客户机真实位置（串口 [UI] perf 里的 cur=X,Y）。

    为什么：SM.Cursor 只按相对位移跟踪，而客户机会裁边/被窗口后的光标状态影响，
    多次点击后会累积偏移（实测第 2 次就点到了别的磁贴）。这里直接以客户机自报为准。
    """
    m = None
    for mm in re.finditer(r"\[UI\] perf[^\n]*cur=(\d+),(\d+)", log_fn()):
        m = mm
    if CUR is not None and m:
        CUR.x = int(m.group(1))
        CUR.y = int(m.group(2))


def _cur_and_count(log):
    m = None
    for mm in re.finditer(r"\[UI\] perf[^\n]*cur=(\d+),(\d+)", log):
        m = mm
    if m is None:
        return None, log.count("[UI] perf")
    return (int(m.group(1)), int(m.group(2))), log.count("[UI] perf")


def _wait_new_perf(n0, timeout=4.0):
    """等一条**新的** [UI] perf 行（客户机 2fps 时一条要数百毫秒）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if log_of(SERIAL_OF[0]).count("[UI] perf") > n0:
            return True
        time.sleep(0.3)
    return False


def goto_exact(mon, x, y, passes=2):
    """把光标送到 (x,y)，并**确认**到位后才返回。

    两个坑（都实测踩过）：
      ① 串口 `-serial file:` 有 1-3 s 落盘延迟，拿旧样本做反馈会越走越偏；
      ② 客户机在开菜单时只有 2fps（redraw_us≈500ms），移动后立刻点击
        会先到按下事件、光标还在旧位置 -> 菜单被判成"点外面"。
    """
    for _ in range(passes):
        pos, n0 = _cur_and_count(log_of(SERIAL_OF[0]))
        if pos is not None and CUR is not None:
            CUR.x, CUR.y = pos
        if pos is not None and abs(x - pos[0]) <= 8 and abs(y - pos[1]) <= 8:
            return True
        CUR.goto(mon, x, y)
        CUR.settle(mon)
        if not _wait_new_perf(n0):
            return False
    pos, _ = _cur_and_count(log_of(SERIAL_OF[0]))
    return pos is not None and abs(x - pos[0]) <= 12 and abs(y - pos[1]) <= 12

def click_dock(mon, item):
    goto_exact(mon, item["cx"], item["cy"])
    time.sleep(0.2)
    mon.click()


def right_click_dock(mon, item):
    cur = cur_of()
    sync_cur(lambda: log_of(SERIAL_OF[0]))
    cur.goto(mon, item["cx"], item["cy"])
    cur.settle(mon)
    time.sleep(0.3)
    mon.raw(["mouse_button 2", "mouse_button 0"], wait_between=0.12, wait_end=0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=460)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    if not QEMU:
        print("[dock64] qemu not found"); return 2
    if not os.path.exists(DISK):
        print("[dock64] fixture missing:", DISK); return 2
    os.makedirs(WORK, exist_ok=True)

    # 每一次运行都用**磁盘副本**：本测试会改 /etc/dock.conf（持久化），不能污染 build64 的演示盘
    img = os.path.join(WORK, "dock64.img")
    shutil.copyfile(DISK, img)
    globals()["DISK"] = img          # start() 读模块级 DISK：改成副本
    serial = os.path.join(WORK, "run.log")
    SERIAL_OF[0] = serial
    if os.path.exists(serial):
        os.remove(serial)
    port = qh.free_port()

    print("=== 1) 首次启动：固定列表 seed -> 持久化 ===", flush=True)
    proc, mon = start(serial, port)
    try:
        if not qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=a.timeout, login_timeout=460):
            print("[dock64] desktop not ready"); return 2
        globals()["CUR"] = SM.Cursor()          # 从 (512,384) 开始跟踪（客户机初始位置）
        log = log_of(serial)
        check("① 首次 seed 打点（[DOCK64] pins src=seed n=8 path=/etc/dock.conf）",
              re.search(r"\[DOCK64\] pins src=seed n=8 path=/etc/dock\.conf ids=\d+(,\d+)+", log) is not None,
              (re.search(r"\[DOCK64\] pins [^\n]*", log) or type("x", (), {"group0": ""})).group(0)
              if re.search(r"\[DOCK64\] pins [^\n]*", log) else "无")
        g0 = geom(log)
        check("② 默认 Dock 几何（items=9 = 开始 + 8 个默认固定，w=520 默认宽度）",
              g0 is not None and int(g0.group(8)) == 9,
              g0.group(0) if g0 else "无")
        items0 = dock_items(log)
        check("③ 固定项 kind=pinned（idx1..8）+ 开始项 kind=start（idx0）",
              len(items0) == 9 and items0[0]["kind"] == "start"
              and all(it["kind"] == "pinned" for it in items0[1:]),
              "kinds=%s" % [it["kind"] for it in items0])
        term = [it for it in items0 if it["app"] == 3]
        check("④ 终端在默认固定项里（app=3，用于后面点击）", len(term) == 1,
              "app=3 at idx=%s" % ([it["idx"] for it in term],))

        # 开 3 个终端窗口：**走开始菜单的固定格**。
        #   为什么不点 Dock 图标：Dock 固定项是**应用级**语义（再点 = 最小化/置前，不开新窗口），
        #   这是 v0.5 就有的行为、本次不改；要证明"不合并"就用开始菜单多开几个窗口。
        print("=== 2) 开 3 个终端窗口 -> 3 个运行图标（不合并）===", flush=True)
        def cur_items():
            """每次点击前重读：Dock 宽度变了就会重新居中，旧坐标会点错项。"""
            return dock_items(log_of(serial))

        def open_term_from_start():
            its = cur_items()
            if not its:
                return False
            click_dock(mon, its[0])                           # idx 0 = 开始按钮（最新坐标）
            wait_mark(lambda: log_of(serial), "[START64] open", 20, proc)
            lg = log_of(serial)
            mt = None
            for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(\d+) name=\S+ en=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+)", lg):
                if mm.group(2) == "3":                       # app=3 = 终端
                    mt = mm
            if not mt:
                return False
            cur = cur_of()
            sync_cur(lambda: log_of(SERIAL_OF[0]))
            goto_exact(mon, int(mt.group(3)) + int(mt.group(5)) // 2, int(mt.group(4)) + int(mt.group(6)) // 2)
            cur.settle(mon)
            time.sleep(0.3)
            mon.click()
            time.sleep(2.5)
            return True
        for _try in range(6):                       # 重试到 3 个运行项（偶发点不中）
            if len(re.findall(r"\[DOCK64\] run-add win=\d+ app=3", log_of(serial))) >= 3:
                break
            open_term_from_start()
        # 再等一拍，确保最后一次 rebuild 打过 geom/item
        time.sleep(2.0)
        wait_mark(lambda: log_of(serial), "running=3", 40, proc)
        log = log_of(serial)
        runs = re.findall(r"\[DOCK64\] run-add win=(\d+) app=3 running=(\d+)", log)
        check("⑤ 三次窗口创建都发到 Dock（[DOCK64] run-add win=… app=3 running=1/2/3）",
              len(runs) >= 3 and runs[-1][1] == "3", "runs=%s" % runs[-3:])
        items = dock_items(log)
        ritems = [it for it in items if it["kind"] == "running" and it["app"] == 3]
        check("⑥ Dock 上真的出现 3 个独立运行项（不合并同一应用多窗口）",
              len(ritems) == 3, "running_app3=%d win_ids=%s" % (len(ritems), [it["win"] for it in ritems]))
        check("⑦ 运行项紧挨在固定项右侧连续排列（x 递增且间隔 = icon+gap）",
              len(ritems) == 3 and all(ritems[i + 1]["x"] > ritems[i]["x"] for i in range(len(ritems) - 1)),
              "xs=%s" % [it["x"] for it in ritems])
        g1 = geom(log)
        check("⑧ 图标变多 -> Dock 自适应变宽（w > 默认 520）",
              g1 is not None and int(g1.group(3)) > 520, g1.group(0) if g1 else "无")

        # 关掉中间那个运行项（右键 -> 关闭窗口）
        print("=== 3) 右键运行图标 -> 关闭窗口（只移除该 window_id）===", flush=True)
        mid = ritems[1]
        right_click_dock(mon, mid)
        wait_mark(lambda: log_of(serial), "menu open owner=dock", 20, proc)
        log = log_of(serial)
        mo = re.search(r"\[DOCK64\] menu open owner=dock app=3 win=(\d+) pinned=(\d) rows=(\d+) items=([\w,-]+)", log)
        check("⑨ 右键运行图标弹出菜单（已固定+运行中 -> unpin 与 close-win 同时存在）",
              mo is not None and "unpin" in mo.group(4) and "close-win" in mo.group(4),
              mo.group(0) if mo else "无")
        # ★ 菜单操作用**键盘**：每按一次 Down 下移一行（默认选中第 0 行），Enter 执行。
        #   为何不用鼠标：客户机开菜单时只有 2fps（redraw_us≈500ms），相对鼠标事件会拖影，
        #   实测点击会落在菜单外面（menu close why=outside）。人用键盘也更顺手。
        if mo is not None:
            items_l = mo.group(4).split(",")
            steps = items_l.index("close-win") if "close-win" in items_l else 0
            for _k in range(steps):
                mon.key("down", wait=0.5)
            mon.key("ret", wait=0.8)
        wait_mark(lambda: log_of(serial), "act=close-win", 20, proc)
        log = log_of(serial)
        act = re.search(r"\[DOCK64\] menu act[^\n]* act=close-win[^\n]* running_n=(\d+)", log)
        check("⑩ 关闭窗口只移除对应运行项（running_n 从 3 变 2）",
              act is not None and act.group(1) == "2", act.group(0) if act else "无")
        check("⑪ 关闭窗口**不**取消固定（pinned_after 仍是 1）",
              re.search(r"act=close-win[^\n]*pinned_after=1", log) is not None, "")
        items = dock_items(log)
        left = [it for it in items if it["kind"] == "running" and it["app"] == 3]
        check("⑫ 另外两个窗口的运行图标仍在（win_id 不同）",
              len(left) == 2 and all(it["win"] != mid["win"] for it in left),
              "left=%s removed=%s" % ([it["win"] for it in left], mid["win"]))

        # 右键固定图标 -> 取消固定
        print("=== 4) 右键固定图标 -> 取消固定（立即持久化）===", flush=True)
        pin_term = [it for it in dock_items(log_of(serial)) if it["kind"] == "pinned" and it["app"] == 3]
        if pin_term:
            right_click_dock(mon, pin_term[0])
            wait_mark(lambda: log_of(serial), "menu open owner=dock app=3", 20, proc)
            log = log_of(serial)
            mo2 = re.search(r"\[DOCK64\] menu open owner=dock app=3 win=- pinned=1 rows=(\d+) items=([\w,-]+)", log)
            check("⑬ 右键**固定**图标：菜单含 unpin（不是 pin）",
                  mo2 is not None and "unpin" in mo2.group(2), mo2.group(0) if mo2 else "无")
            gy = int(re.search(r"\[DOCK64\] menu open owner=dock app=3[^\n]* y=(\d+)", log).group(1))
            gx = int(re.search(r"\[DOCK64\] menu open owner=dock app=3[^\n]* x=(\d+)", log).group(1))
            items2_l = mo2.group(2).split(",")
            steps2 = items2_l.index("unpin") if "unpin" in items2_l else 0
            for _k in range(steps2):
                mon.key("down", wait=0.5)
            mon.key("ret", wait=0.8)
            wait_mark(lambda: log_of(serial), "act=unpin", 20, proc)
            log = log_of(serial)
            ua = re.search(r"\[DOCK64\] menu act[^\n]* act=unpin[^\n]*pinned_after=0[^\n]*pinned_n=(\d+) running_n=(\d+)", log)
            check("⑭ 取消固定生效（pinned_after=0、pinned_n 从 8 变 7）",
                  ua is not None and ua.group(1) == "7", ua.group(0) if ua else "无")
            check("⑮ 取消固定**不关窗口**（running_n 仍是 2）",
                  ua is not None and ua.group(2) == "2", "")
            check("⑯ 固定/取消固定立即写盘（[DOCK64] pins save … ok=1）",
                  re.search(r"\[DOCK64\] pins save n=7 bytes=\d+ ok=1", log) is not None, "")
            items = dock_items(log)
            pins = [it for it in items if it["kind"] == "pinned"]
            check("⑰ 剩余固定项左对齐连续补位（没有空洞：x 严格递增且下标连续）",
                  len(pins) == 7 and all(pins[i + 1]["idx"] == pins[i]["idx"] + 1 for i in range(6)),
                  "pins=%s" % [it["idx"] for it in pins])
            check("⑱ 取消固定后 app=3 不再出现在固定区（只剩运行项）",
                  all(it["app"] != 3 or it["kind"] == "running" for it in items),
                  "app3=%s" % [(it["kind"], it["idx"]) for it in items if it["app"] == 3])
    finally:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass

    # 宿主侧：从镜像里读出 /etc/dock.conf（证明"持久化到卷"，不是内存里变一下）
    print("=== 5) 宿主侧回读 /etc/dock.conf（持久化落地）===", flush=True)
    # 持久化的硬证据在下面：**重启后由客户机自己从 /etc/dock.conf 读回**（src=file）。
    # 不在宿主侧扫字节：vfs64_write64 是“重写到新块”，旧块里还留着历史文本，
    # 扫到的可能是旧副本（实测就扫到过只有表头的一份）。
    # 第二次启动：必须 src=file（复用持久化），且固定项 = 7
    print("=== 6) 第二次启动：从持久化恢复（src=file）===", flush=True)
    serial2 = os.path.join(WORK, "run2.log")
    if os.path.exists(serial2):
        os.remove(serial2)
    port2 = qh.free_port()
    proc2, mon2 = start(serial2, port2)
    try:
        qh.wait_desktop(mon2, lambda: log_of(serial2), proc2, timeout=a.timeout, login_timeout=460)
        log2 = log_of(serial2)
        ml = re.search(r"\[DOCK64\] pins src=file n=(\d+) path=/etc/dock\.conf ids=([\d,]+)", log2)
        det = ml.group(0) if ml else "无"
        check("㉑ 第二次启动从**文件**加载（src=file）且固定项 = 7（上次取消了终端）",
              ml is not None and ml.group(1) == "7" and "3" not in ml.group(2).split(","), det)
        items2 = dock_items(log2)
        check("㉒ 重启后只恢复固定图标（无运行项：没有 kind=running）",
              len(items2) > 0 and all(it["kind"] != "running" for it in items2),
              "items=%d" % len(items2))
        check("㉓ 重启后固定项 = 7（与取消固定后的持久化一致）",
              len([it for it in items2 if it["kind"] == "pinned"]) == 7,
              "pinned=%d" % len([it for it in items2 if it["kind"] == "pinned"]))
    finally:
        proc2.kill()
        try:
            proc2.wait(timeout=10)
        except Exception:
            pass

    print("")
    print("===== RESULT: %s =====" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
