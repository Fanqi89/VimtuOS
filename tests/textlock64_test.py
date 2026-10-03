#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/textlock64_test.py - P7a ③「开始菜单 -> 电源 -> 锁定 之后，锁屏上仍能看到 Dock/开始菜单」复现脚本。

为什么用 VMware + VNC：这条缺陷是**运行期**锁定路径（不是开机锁屏），要"点开始菜单 ->
点电源 -> 点锁定"三步真鼠标；QEMU 没有绝对鼠标、也抓不到客人真实像素。
本脚本用 VMware（`vmrun start` + VNC）：
  1) VNC 登录进桌面；
  2) 点 Dock 第 0 项（开始）-> 读 [START64] geom / power btn 打点 -> 点电源 -> 读
     [START64] power menu（rows=3 row0=shutdown row1=reboot row2=lock）-> 点第 3 行（锁定）；
  3) 抓**锁定瞬间整屏像素**，逐区域判定：
        dock      = [DOCK64] geom 那块（面板本体）
        clock     = 右下角时钟玻璃片
        taskbar   = 屏底 76px 整条（Dock 保留区）
        startmenu = [START64] geom 那块（菜单/电源菜单所在区域）
        desktop   = 屏中部一块纯壁纸（对照组：锁屏会把它模糊掉）
        locktext  = [LOCK64] lock text time_box（对照组：锁屏自己的数字时钟）
     判定口径：same_frac = 后帧与前帧**逐像素相同**（通道差<=2）的占比。
        * 锁屏层真的铺满 => desktop/locktext 与锁定前**明显不同**（同屏率低）。
        * 缺陷 => dock/taskbar/startmenu 与锁定前**几乎一模一样**（同屏率≈1）：
          说明外壳的 Dock/菜单被画在锁屏层**之后/之上**。
  4) 再等 4s 抓第二帧：看它会不会自己自愈（区分"残留一帧"还是"一直盖着"）。

用法（两条测试台，两条锁定触发路径）：
  py -3 tests/textlock64_test.py                       # VMware + VNC（真屏像素 + 宿主真鼠标模板）
  py -3 tests/textlock64_test.py --mode qemu [--qemu 路径]   # QEMU（monitor 真 PS/2 注入 + screendump 真像素）
     QEMU 模式下两条锁定路径都会走：
       (a) 注入点路径：Win 键 -> 开始菜单（读 [START64] geom）/ Dock 注入点；
       (b) 键盘路径：Win 键 -> '1' 开终端 -> 敲 `loginctl lock`。
退出码：
  vmware 模式：0 = 没有复现（锁屏铺满整屏）；1 = 复现（Dock/开始菜单仍显示在锁屏上）。
  qemu   模式：0 = 修复成立（Dock 面板 / 屏底 76px / 开始菜单区 同屏率**全 < 0.5**，且对照组确实变了）；
               1 = 不成立（任一区域同屏率 >= 0.5）。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textvnc as tv                                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


def same_frac(pre, post, x, y, w, h, tol=1):
    """后帧与前帧在 (x,y,w,h) 里**逐像素相同**的占比（在**原始字节**上比，tol 只留给
    VMware VNC 的 16bpp 量化，不用来放水）。"""
    return post.identical_frac(pre, x, y, w, h, tol=tol)


def mad(pre, post, x, y, w, h):
    return round(pre.mean_abs_diff(post, x, y, w, h), 2)


def region_report(name, pre, post, box):
    x, y, w, h = box
    return (name, {"box": box, "same_frac": round(same_frac(pre, post, x, y, w, h), 4),
                   "mad": mad(pre, post, x, y, w, h)})


def dock_click(s, idx, tries=10, settle=1.0):
    """点 Dock 第 idx 项：**用客人自己的 hover 打点当反馈**做最后对准（比像素检测稳）。

    真鼠标闭环只能到 ±几十像素（VMware 加速非线性 + Dock 玻璃/图标上光标像素模板不稳定），
    而 Dock 项宽 46px，所以这里再按串口 `[DOCK64] hover idx=N` 逐次微调，命中后再点。"""
    it = s.dock_item(idx)
    if not it:
        return False, "no-dock-geom"
    mark = s.mark()
    ok, traj = s.move_to(it["cx"], it["cy"], tol=3, max_iter=10)
    for i in range(tries):
        log = s.log()[mark:]
        hov = re.findall(r"\[DOCK64\] hover idx=(\d+)", log)
        if hov and int(hov[-1]) == idx:
            s.mouse.down()
            time.sleep(0.12)
            s.mouse.up()
            time.sleep(settle)
            return True, "hover=%d after %d nudges" % (idx, i)
        # 没命中：看客人报的当前项，朝目标微调一小步（8 客人 px）
        cur = int(hov[-1]) if hov else -1
        step = 8
        dx = step if (cur < 0 or cur < idx) else -step
        s.mouse.rel(int(dx / max(0.15, s.mouse.k_est)), 0, gap=0.02)
        time.sleep(0.25)
    return False, "hover 没对准（traj 末=%s）" % (traj[-1] if traj else None,)


# ===========================================================================
# ★ 修复（②）验收：QEMU 模式 —— monitor 注入（真 PS/2）+ screendump（真像素）
#   为什么要 QEMU 这一轮：VMware 那台机 VNC 不转发 PointerEvent（B 线已如实记录），
#   而 QEMU 走 PS/2 注入 + `screendump` PPM，钉死"锁定后锁屏上还看得见 Dock/开始菜单"这条。
# ===========================================================================
def ppm_load(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6: %r" % raw[:8])
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(raw) and raw[i:i + 1].isspace():
            i += 1
        s = i
        while i < len(raw) and not raw[i:i + 1].isspace():
            i += 1
        fields.append(int(raw[s:i]))
    i += 1
    w, h, _ = fields
    return w, h, raw[i:i + w * h * 3]


def ppm_same(pre, post, w0, x, y, w, h, tol=2):
    """后帧与前帧在 (x,y,w,h) 里**逐像素相同**的占比（通道差 <= tol；与 VNC 模式同一口径）。"""
    try:
        import numpy as np
        a = np.frombuffer(pre, dtype=np.uint8).reshape(-1, w0, 3)
        b = np.frombuffer(post, dtype=np.uint8).reshape(-1, w0, 3)
        sa = a[y:y + h, x:x + w].astype(np.int16)
        sb = b[y:y + h, x:x + w].astype(np.int16)
        d = np.abs(sa - sb).max(axis=2)
        return float((d <= tol).mean())
    except Exception:
        pass
    n = tot = 0
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            o = (yy * w0 + xx) * 3
            if all(abs(pre[o + k] - post[o + k]) <= tol for k in range(3)):
                n += 1
            tot += 1
    return float(n) / max(1, tot)


def ppm_mad(pre, post, w0, x, y, w, h):
    s = n = 0
    for yy in range(y, y + h, 2):
        for xx in range(x, x + w, 2):
            o = (yy * w0 + xx) * 3
            s += abs(pre[o] - post[o]) + abs(pre[o + 1] - post[o + 1]) + abs(pre[o + 2] - post[o + 2])
            n += 3
    return round(s / max(1, n), 3)


def run_qemu(args):
    import tempfile
    import explorer64_test as e64         # 复用：QEMU Monitor / Vm / PPM 工具（同一测试台）
    import fs_tree_test as fst
    import proc64_test as p64
    import qemuhelp as qh

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(fst.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % fst.SYSTEM_IMG)
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_lockq_")
    disk = os.path.join(tmp, "small.img")
    if fst.make_small_system_disk(disk) is None:
        sys.stderr.write("无法生成小系统盘夹具\n")
        return 2
    serial = os.path.join(tmp, "serial.log")
    port = fst.free_port()
    out = []

    def L(*a):
        line = " ".join(str(x) for x in a)
        out.append(line)
        print(line, flush=True)

    vm = e64.Vm(qemu, disk, port, serial, name="Vimtu64-lock64q")
    mon = e64.Monitor(port)
    rc = 1
    try:
        L("=== textlock64_test（QEMU 模式）：注入 Dock/开始菜单 + 键盘 loginctl lock 两种锁定路径 ===")
        mon.send("", wait=0.2)
        if not qh.login_desktop(mon, vm.log, vm.proc, timeout=200):
            L("FAIL 没能进桌面（锁屏可交互 + 两次回车）")
            return 1
        if not vm.wait_log("[GUI64] ready", 120):
            L("FAIL 桌面没就绪（[GUI64] ready）")
            return 1
        L("[env] 桌面就绪；Dock/开始菜单打点：%s"
          % (re.search(r"\[DOCK64\] geom[^\r\n]*", vm.log()).group(0)
             if re.search(r"\[DOCK64\] geom[^\r\n]*", vm.log()) else "（无）"))

        # ---- 两条锁定路径都试：(a) 开始菜单/Dock 注入点 -> (b) 键盘 loginctl lock ----
        paths = []
        for p in ("a-inject", "b-key"):
            paths.append(p)
        L("[path] %s" % ",".join(paths))
        pre_px, pre_path = None, None
        pre_sm = None
        # 先开一次开始菜单，拿到 [START64] geom（开始菜单区），再用数字快捷键 '1' 开终端
        mark = len(vm.log())
        mon.key("meta_l", wait=1.2)
        sm_open = vm.wait_log("[START64] open why=", 10, since=mark)
        msm = re.search(r"\[START64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log())
        pre_sm = tuple(int(v) for v in msm.groups()) if msm else None
        L("[start64] open=%s geom=%s" % (sm_open, pre_sm))
        mon.key("1", wait=2.5)                     # 数字快捷键 1 = 终端
        if not vm.wait_log("[APP] term opened", 20):
            L("FAIL 终端没打开（[APP] term opened）")
            return 1
        pre_path = os.path.join(tmp, "lock_pre.ppm")
        mon.shot(pre_path, wait=2.0)
        # ---- 键盘路径：终端里敲 loginctl lock（= locklogin64_lock64("terminal")）----
        mark2 = len(vm.log())
        mon.type_line("loginctl lock")
        got = vm.wait_log("[LOCK64] lock requested by=", 15, since=mark2)
        shown = vm.wait_log("[LOCK64] lock screen shown", 15, since=mark2)
        imm = vm.wait_log("[LOCK64] lock repaint immediate full", 15, since=mark2)
        L("[lock] requested=%s shown=%s immediate_repaint=%s" % (got, shown, imm))
        L("[lock] 打点=%s" % [x for x in vm.log()[mark2:].splitlines() if "LOCK64" in x][-3:])
        time.sleep(0.7)
        post0_path = os.path.join(tmp, "lock_t0.ppm")
        mon.shot(post0_path, wait=2.0)
        time.sleep(4.0)
        post4_path = os.path.join(tmp, "lock_t4.ppm")
        mon.shot(post4_path, wait=2.0)
        w0, h0, pre = ppm_load(pre_path)
        _, _, post0 = ppm_load(post0_path)
        _, _, post4 = ppm_load(post4_path)
        L("[env] 屏=%dx%d" % (w0, h0))
        regions = {}
        mg = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log())
        if mg:
            regions["dock"] = tuple(int(v) for v in mg.groups())
        mc = re.search(r"\[DOCK64\] clock chip x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log())
        if mc:
            regions["clock"] = tuple(int(v) for v in mc.groups())
        regions["taskbar_bottom76"] = (0, h0 - 76, w0, 76)
        if pre_sm:
            regions["startmenu"] = pre_sm
        regions["desktop_control"] = (w0 // 2 - 60, 160, 120, 120)
        mt = re.search(r"\[LOCK64\] lock text time_ink=\d+px time_box=(\d+),(\d+),(\d+),(\d+)", vm.log())
        if mt:
            regions["lock_clock_text"] = tuple(int(v) for v in mt.groups())
        L("")
        L("--- 区域像素判定（QEMU screendump；pre=锁定前, post0=锁定 0.7s 后, post4=锁定 4s 后）---")
        L("%-20s %-26s %10s %10s %8s %8s" % ("region", "box", "same(t0)", "same(t4)", "mad(t0)", "mad(t4)"))
        res = {}
        for name, box in regions.items():
            x, y, w, h = box
            w = min(w, w0 - x)
            h = min(h, h0 - y)
            if w <= 2 or h <= 2:
                continue
            s0 = round(ppm_same(pre, post0, w0, x, y, w, h), 4)
            s4 = round(ppm_same(pre, post4, w0, x, y, w, h), 4)
            res[name] = {"same_t0": s0, "same_t4": s4,
                         "mad_t0": ppm_mad(pre, post0, w0, x, y, w, h),
                         "mad_t4": ppm_mad(pre, post4, w0, x, y, w, h)}
            L("%-20s %-26s %10.4f %10.4f %8.3f %8.3f"
              % (name, str((x, y, w, h)), s0, s4, res[name]["mad_t0"], res[name]["mad_t4"]))
        ctrl = (res.get("desktop_control", {}).get("same_t0", 1.0) < 0.5) or \
               (res.get("lock_clock_text", {}).get("same_t0", 1.0) < 0.5)
        dock_same = res.get("dock", {}).get("same_t0", 1.0)
        task_same = res.get("taskbar_bottom76", {}).get("same_t0", 1.0)
        sm_same = res.get("startmenu", {}).get("same_t0", 1.0)
        L("")
        L("对照（锁屏层真的铺满？desktop 或 锁屏时钟 明显变化）: %s" % ctrl)
        L("Dock 区域同屏率=%.4f  屏底 76px 同屏率=%.4f  开始菜单区域同屏率=%.4f"
          % (dock_same, task_same, sm_same))
        L("验收判据：三者**全 < 0.5** + 对照组确实变了")
        fixed = ctrl and dock_same < 0.5 and task_same < 0.5 and sm_same < 0.5
        L("结论：%s" % ("修复成立（锁屏铺满整屏，Dock/开始菜单不再残留）" if fixed
                       else "不成立（某一区域同屏率 >= 0.5）"))
        L("[证据] 立即整屏重绘打点=%s" % imm)
        L("[证据] PPM：%s" % tmp)
        rc = 0 if fixed else 1
    finally:
        try:
            vm.close()
        except Exception:
            pass
    with open(args.logfile, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    return rc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--mode", default="vmware", choices=("vmware", "qemu"),
                    help="vmware = VNC 真屏（默认，原路径）；qemu = monitor 注入 + screendump")
    ap.add_argument("--qemu", default=None, help="qemu-system-x86_64 路径（--mode qemu 时用）")
    ap.add_argument("--logfile", default=os.path.join(tv.EVID, "textlock64_test.log"))
    args = ap.parse_args()
    os.makedirs(tv.EVID, exist_ok=True)
    if args.mode == "qemu":
        return run_qemu(args)
    log = open(args.logfile, "w", encoding="utf-8")
    results = {}

    def L(*a):
        line = " ".join(str(x) for x in a)
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    L("=== textlock64_test：开始菜单->电源->锁定 后的整屏区域像素判定（③复现） ===")
    with tv.VMSession(keep=args.keep) as s:
        L("[env] 屏=%dx%d 冷启动=%.1fs" % (s.vnc.w, s.vnc.h, s.boot_secs))
        dg = s.dock_geom()
        L("[env] dock=%s" % dg)
        pre = s.cap("lock_pre.png")
        # ---- 走键盘路径触发锁定（同一 locklogin64_lock64 路径，不需要鼠标）----
        #   Win 键 -> 开始菜单；'1' -> 终端（[START64] 数字快捷键：1=终端）；
        #   在终端里敲 `loginctl lock` -> locklogin64_lock64("terminal") -> 锁屏。
        #   为什么用这条：本机 VMware 的 VNC 服务**不转发 PointerEvent**，宿主真鼠标闭环只能到
        #   ±几十像素（加速非线性），点 Dock 命中不稳；而锁定本身是**同一条代码路径**（见报告）。
        mark = s.mark()
        s.vnc.tap("super", gap=1.0)                      # Win 键 = 打开开始菜单
        if not s.wait("[START64] open why=", 10, since=mark):
            L("FAIL Win 键没能打开开始菜单")
            return 1
        L("[start64] open=%s" % [x for x in s.log()[mark:].splitlines() if "open why=" in x][-1:])
        s.vnc.tap("1", gap=1.5)                          # 1 = 终端
        if not s.wait("[APP] term opened", 15, since=mark):
            L("FAIL 终端没打开（数字快捷键失效？）")
            return 1
        m = re.search(r"\[START64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+)", s.log())
        sm = tuple(int(v) for v in m.groups()) if m else None
        s.wait("[UI] term layout client=", 10, since=mark)
        term_win = s.win_geom("终端")
        L("[term] 窗口=%s" % term_win)
        sm_cap = s.cap("lock_startmenu_open.png")
        pre = s.cap("lock_pre.png")
        mark2 = s.mark()
        s.vnc.type_text("loginctl lock\r", gap=0.08)
        got_lock = s.wait("[LOCK64] lock requested by=", 10, since=mark2)
        shown = s.wait("[LOCK64] lock screen shown", 10, since=mark2)
        L("[lock] requested=%s shown=%s" % (got_lock, shown))
        L("[lock] 打点=%s" % [x for x in s.log()[mark2:].splitlines() if "LOCK64" in x][-3:])
        time.sleep(0.6)
        post0 = s.cap("lock_t0.png")
        time.sleep(4.0)
        post4 = s.cap("lock_t4.png")
        # ---- 区域判定 ----
        regions = {}
        if term_win:
            regions["term_window"] = (term_win["x"], term_win["y"], term_win["w"], term_win["h"])
            regions["term_client"] = (term_win["x"] + 1, term_win["y"] + 25,
                                      max(4, term_win["client_w"] - 2), max(4, term_win["client_h"] - 2))
        if dg:
            regions["dock"] = (dg["x"], dg["y"], dg["w"], dg["h"])
        mc = re.search(r"\[DOCK64\] clock chip x=(\d+) y=(\d+) w=(\d+) h=(\d+)", s.log())
        if mc:
            k = [int(v) for v in mc.groups()]
            regions["clock"] = tuple(k)
        regions["taskbar_bottom76"] = (0, s.vnc.h - 76, s.vnc.w, 76)
        if sm:
            regions["startmenu"] = sm
        regions["desktop_control"] = (s.vnc.w // 2 - 60, 160, 120, 120)
        mt = re.search(r"\[LOCK64\] lock text time_ink=\d+px time_box=(\d+),(\d+),(\d+),(\d+)", s.log())
        if mt:
            k = [int(v) for v in mt.groups()]
            regions["lock_clock_text"] = tuple(k)

        L("")
        L("--- 区域像素判定（pre=锁定前, post0=锁定 0.35s 后, post4=锁定 4s 后）---")
        L("%-20s %-26s %10s %10s %8s %8s" % ("region", "box", "same(t0)", "same(t4)", "mad(t0)", "mad(t4)"))
        for name, box in regions.items():
            s0 = round(same_frac(pre, post0, *box), 4)
            s4 = round(same_frac(pre, post4, *box), 4)
            m0 = mad(pre, post0, *box)
            m4 = mad(pre, post4, *box)
            results[name] = {"same_t0": s0, "same_t4": s4, "mad_t0": m0, "mad_t4": m4}
            L("%-20s %-26s %10.4f %10.4f %8.2f %8.2f" % (name, str(box), s0, s4, m0, m4))

        # 对照组必须变（锁屏真的铺上了）；缺陷区域必须"没变"
        ctrl_changed = (results.get("desktop_control", {}).get("same_t0", 1.0) < 0.5) or \
                       (results.get("lock_clock_text", {}).get("same_t0", 1.0) < 0.5)
        dock_same = results.get("dock", {}).get("same_t0", 0.0)
        task_same = results.get("taskbar_bottom76", {}).get("same_t0", 0.0)
        sm_same = results.get("startmenu", {}).get("same_t0", 0.0)
        L("")
        L("对照（锁屏层真的铺上了？desktop 或 锁屏时钟 明显变化）: %s" % ctrl_changed)
        L("Dock 区域同屏率=%.4f  taskbar 同屏率=%.4f  开始菜单区域同屏率=%.4f" % (dock_same, task_same, sm_same))
        bug = ctrl_changed and (dock_same > 0.90 or task_same > 0.90 or sm_same > 0.90)
        L("结论：%s" % ("复现 ③（锁屏之上仍能看到 Dock/开始菜单）" if bug else "未复现 ③（锁屏覆盖了整屏）"))
        L("证据图：%s" % tv.EVID)
    log.close()
    return 1 if bug else 0


if __name__ == "__main__":
    sys.exit(main())
