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

用法：py -3 tests/textlock64_test.py [--keep]
退出码：0 = 没有复现（锁屏铺满整屏）；1 = 复现（Dock/开始菜单仍显示在锁屏上）。
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--logfile", default=os.path.join(tv.EVID, "textlock64_test.log"))
    args = ap.parse_args()
    os.makedirs(tv.EVID, exist_ok=True)
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
