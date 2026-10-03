#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/textwm64_test.py - P7a ①②「窗口内容不随尺寸变化 / 不随移动」的**复现脚本**。

为什么用 VMware + VNC（不是 QEMU）：
  这两条缺陷是**连续拖拽**（拖边/拖角缩放、拖标题栏移动）触发的；QEMU 路径只有
  keyboard 注入（monitor sendkey），没有绝对鼠标，上一批因此判"没复现"。
  本脚本走 VMware：`vmrun start` + VNC 抓**客人真实整屏像素** + RFB PointerEvent 做**真鼠标**
  （按下-多步移动-松开），并用「光标像素模板闭环」保证落点准（不靠 VMware 加速曲线）。

判定口径（全部是像素数，不靠肉眼）：
  * MOVE：窗口移动 (dx,dy) 后，客户区**相对窗口原点**的内容必须不变：
        moved = MAD(前帧[客户区原点+p], 后帧[移动后客户区原点+p])   -> 越小越好
        stuck = MAD(前帧[客户区原点+p], 后帧[同一个屏幕位置+p])     -> 越大越好（说明没留在原地）
    结论 MOVED_OK 需要 moved 明显小于 stuck；若 stuck≈0 且 moved 大 => 内容留在原地（缺陷）。
  * RESIZE：串口 `[UI] win resize end dir=.. w= h= client=WxH` 证明几何真的变了；再看
    **新多出来的那条带**（右带 dw、下带 dh，都在新客户区内）：
        uniform_frac = 该带里"同一个颜色"的像素占比
        bg_frac      = 该带里恰好等于外壳客户区底色 rgb(240,240,240) 的占比
    正确行为：应用按新客户区重绘 -> uniform_frac 明显 <0.98（带里有内容/边框/文字）。
    缺陷行为：应用布局写死在 660x470（explorer64），新区域一个像素都没画 -> bg_frac≈1.0。
  另外把每个应用**自己的重排打点**（[UI] term layout / [SET64] layout / [UI] tmgr layout）
  和拖拽打点（[UI] win resize dir/end）如实打印出来，作为"接没接重排回调"的证据。

用法：
  py -3 tests/textwm64_test.py [--apps explorer,terminal,settings,taskmgr] [--reps 3]
                              [--actions move,resize] [--keep] [--vmx ...]
退出码：0 = 全部判定通过；1 = 至少一条缺陷复现（或有动作没做成功）。
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

CLIENT_BG = (240, 240, 240)      # theme64.cpp：client_bg = rgb(240,240,240)（浅/深主题都一样）

# Dock 下标见 gui64.cpp kDockItems：0 开始 / 1 我的电脑(explorer) / 3 终端 / 6 设置 / 7 任务管理器
APPS = {
    "explorer": {"dock": 1, "title": "文件资源管理器", "app": 7, "layout_tag": None},
    "terminal": {"dock": 3, "title": "终端", "app": 3, "layout_tag": r"\[UI\] term layout client="},
    "settings": {"dock": 6, "title": "设置", "app": 5, "layout_tag": r"\[(SET64|UI) settings? layout"},
    "taskmgr":  {"dock": 7, "title": "任务管理器", "app": 4, "layout_tag": r"\[UI\] tmgr layout"},
}


def mad(frame_a, frame_b, x, y, w, h, ox=0, oy=0):
    """平均绝对差（MAD，三通道平均；0 = 逐像素完全一样）。"""
    return frame_a.mean_abs_diff(frame_b, x, y, w, h, ox, oy)



def probes(box, patch=24):
    """客户区内部 9 个固定偏移探针（相对客户区左上角；避开边缘/标题栏）。"""
    w, h = box["w"], box["h"]
    if w < 4 * patch or h < 4 * patch:
        patch = max(8, min(w, h) // 6)
    xs = [8, max(8, w // 2 - patch // 2), max(8, w - patch - 8)]
    ys = [8, max(8, h // 2 - patch // 2), max(8, h - patch - 8)]
    return [(x, y, patch) for y in ys for x in xs]



def move_test(s, name, rep, f_before):
    """拖标题栏移动 -> 像素判定。

    moved = MAD(前帧[客户区原点+p], 后帧[客户区原点+(dx,dy)+p])  = 内容跟着窗口走
    stuck = MAD(前帧[客户区原点+p], 后帧[同一屏幕位置+p])        = 内容留在原地
    """
    win = s.win_geom(APPS[name]["title"])
    if not win:
        return "NO_WIN", {}
    scr_w, scr_h = s.vnc.w, s.vnc.h
    tx = 8 if win["x"] > scr_w // 2 else max(8, min(scr_w - win["w"] - 8, win["x"] + 240))
    ty = 12 if win["y"] > 200 else max(12, min(scr_h - 76 - win["h"] - 8, win["y"] + 150))
    dx, dy = tx - win["x"], ty - win["y"]
    if dx == 0 and dy == 0:
        dx = -180
    px, py = tv.title_drag_point(win)
    ok, tr = s.drag(px, py, px + dx, py + dy, steps=16)
    if not ok:
        return "DRAG_FAIL", {"traj": tr[-3:]}
    time.sleep(1.0)
    s.cap()                                   # 掉一帧
    f_after = s.cap("%s_move_r%d_after.png" % (name, rep))
    c0 = (win["x"] + 1, win["y"] + 25)
    pr = probes({"w": win["client_w"], "h": win["client_h"]})
    moved = [f_before.mean_abs_diff(f_after, c0[0] + ox, c0[1] + oy, p, p, ox=dx, oy=dy)
             for (ox, oy, p) in pr]
    stuck = [f_before.mean_abs_diff(f_after, c0[0] + ox, c0[1] + oy, p, p)
             for (ox, oy, p) in pr]
    m = sum(moved) / len(moved)
    st = sum(stuck) / len(stuck)
    nums = {"dx": dx, "dy": dy, "moved_mad": round(m, 2), "stuck_mad": round(st, 2),
            "moved_worst": round(max(moved), 2), "win_before": (win["x"], win["y"]),
            "win_expect": (tx, ty), "cursor_traj_ok": True}
    if m <= 4.0 and st > 3.0 * max(m, 0.5):
        return "MOVED_OK", nums
    if st <= 1.5 and m > 4.0:
        return "CONTENT_STUCK", nums
    if m <= 4.0 and st <= 4.0:
        return "NO_MOVE(window 没动)", nums
    return "AMBIGUOUS", nums


def resize_test(s, name, rep, f_before):
    """拖右下角缩放 -> 像素判定 + 串口重排打点。"""
    win = s.win_geom(APPS[name]["title"])
    if not win:
        return "NO_WIN", {}
    scr_w, scr_h = s.vnc.w, s.vnc.h
    dw = 120 if win["x"] + win["w"] + 120 < scr_w else -120
    dh = 90 if win["y"] + win["h"] + 90 < scr_h - 76 else -90
    gx, gy = tv.resize_grip_point(win)
    mark = s.mark()
    ok, tr = s.drag(gx, gy, gx + dw, gy + dh, steps=18)
    if not ok:
        return "DRAG_FAIL", {"traj": tr[-3:]}
    time.sleep(1.2)
    s.cap()
    f_after = s.cap("%s_resize_r%d_after.png" % (name, rep))
    log_new = s.log()[mark:]
    win2 = s.win_geom(APPS[name]["title"])
    geo_lines = [l for l in log_new.splitlines()
                 if re.search(r"\[UI\] win resize (dir|end)|layout client=", l)]
    nums = {"dw": dw, "dh": dh,
            "win_after": (win2["x"], win2["y"], win2["w"], win2["h"]) if win2 else None,
            "geo_log": geo_lines[-4:]}
    if not win2:
        return "NO_WIN", nums
    if (win2["w"], win2["h"]) == (win["w"], win["h"]):
        nums["why"] = "几何没变（拖拽没抓到边/角）"
        return "NO_RESIZE", nums
    c0 = (win["x"] + 1, win["y"] + 25)
    c1 = (win2["x"] + 1, win2["y"] + 25)
    # 新多出来的带（右带 + 下带），坐标都在**新窗口**里
    bands = []
    if win2["client_w"] > win["client_w"]:
        bands.append((c1[0] + win["client_w"] + 2, c1[1] + 4, win2["client_w"] - win["client_w"] - 4,
                      win2["client_h"] - 8))
    if win2["client_h"] > win["client_h"]:
        bands.append((c1[0] + 4, c1[1] + win["client_h"] + 2, win2["client_w"] - 8,
                      win2["client_h"] - win["client_h"] - 4))
    if not bands:
        nums["why"] = "新客户区没有变大"
        return "NO_GROW", nums
    uni, dom_colors = [], []
    for (bx, by, bw, bh) in bands:
        if bw <= 2 or bh <= 2:
            continue
        v, frac = f_after.dominant_raw(bx, by, bw, bh)
        uni.append(frac)
        dom_colors.append(v)
    if not uni:
        nums["why"] = "带宽太小，没法判"
        return "AMBIGUOUS", nums
    nums["band_uniform"] = round(sum(uni) / len(uni), 4)
    nums["band_dominant_raw"] = [hex(v) for v in dom_colors]
    # ★ 自我标定：外壳客户区底色 = theme64.cpp 的 client_bg = rgb(240,240,240)。
    #   把"带里最主要的那个原始像素值"按 565 / 565+R/B 互换两种解读解码，哪种能给出
    #   (240,240,240) 就说明是哪一种映射 —— 同时也是"这条带 == 外壳只铺了 client_bg"的硬证据。
    bg_hits = []
    for v in dom_colors:
        a, b = tv.raw_to_rgb565(v)
        bg_hits.append(a == CLIENT_BG or b == CLIENT_BG)
    nums["band_is_client_bg"] = all(bg_hits) and len(bg_hits) > 0
    nums["client_before"] = (win["client_w"], win["client_h"])
    nums["client_after"] = (win2["client_w"], win2["client_h"])
    inter = f_before.mean_abs_diff(f_after, c0[0] + 8, c0[1] + 8, min(win["client_w"], win2["client_w"]) - 16,
                                   min(win["client_h"], win2["client_h"]) - 16)
    nums["overlap_mad"] = round(inter, 2)
    if nums["band_is_client_bg"] and nums["band_uniform"] >= 0.985:
        return "STRIP_NOT_PAINTED(新客户区只被外壳 client_bg 填)", nums
    if nums["band_uniform"] >= 0.98:
        return "STRIP_UNIFORM(新客户区一片纯色)", nums
    return "REPAINT_OK", nums


def _unique_rows(flat):
    """返回 (唯一行, 计数) —— 用 numpy 做，避免 4 万像素的 python 循环。"""
    import numpy as np
    v = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    cols, counts = np.unique(v, return_counts=True)
    return cols, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apps", default="explorer,terminal,settings,taskmgr")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--actions", default="move,resize")
    ap.add_argument("--keep", action="store_true", help="跑完不停 VM（调试用）")
    ap.add_argument("--vmx", default=tv.VMX_BOOT)
    ap.add_argument("--logfile", default=os.path.join(tv.EVID, "textwm64_test.log"))
    args = ap.parse_args()
    os.makedirs(tv.EVID, exist_ok=True)
    log = open(args.logfile, "w", encoding="utf-8")

    def L(*a):
        line = " ".join(str(x) for x in a)
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    names = [x.strip() for x in args.apps.split(",") if x.strip()]
    actions = [x.strip() for x in args.actions.split(",") if x.strip()]
    L("=== textwm64_test：VMware 真鼠标 + VNC 真像素（①②复现） ===")
    results = []
    with tv.VMSession(vmx=args.vmx, keep=args.keep) as s:
        L("[env] 屏=%dx%d 冷启动到锁屏=%.1fs 桌面=%.1fs" %
          (s.vnc.w, s.vnc.h, s.boot_secs, getattr(s, "t_desktop", -1)))
        dg = s.dock_geom()
        L("[env] dock=%s" % dg)
        for name in names:
            info = APPS[name]
            win = s.open_dock_app(info["dock"], info["title"])
            if not win:
                L("[%s] FAIL 开窗失败（dock idx=%d）" % (name, info["dock"]))
                results.append((name, "-", "OPEN_FAIL"))
                continue
            L("[%s] 开窗 ok title=%s x=%d y=%d w=%d h=%d client=%dx%d" %
              (name, win["title"], win["x"], win["y"], win["w"], win["h"],
               win["client_w"], win["client_h"]))
            time.sleep(0.8)
            for rep in range(1, args.reps + 1):
                for act in actions:
                    cur = s.win_geom(info["title"])
                    if not cur:
                        L("[%s] r%d %s SKIP 找不到窗口" % (name, rep, act))
                        continue
                    f_before = s.cap("%s_%s_r%d_before.png" % (name, act, rep))
                    if act == "move":
                        vd, nums = move_test(s, name, rep, f_before)
                    else:
                        vd, nums = resize_test(s, name, rep, f_before)
                    L("[%s] r%d %s -> %s  %s" % (name, rep, act, vd, nums))
                    results.append((name, act, vd))
            # 关窗（点标题栏最右的关闭按钮），避免下一个应用的判定被上一个窗口盖住
            w4 = s.win_geom(info["title"])
            if w4:
                cx, cy = tv.close_button(w4)
                s.click(cx, cy, settle=1.0)
                time.sleep(0.6)
                L("[%s] 关窗尝试后仍在? %s" % (name, bool(s.win_geom(info["title"]))))
        L("")
        L("=== 判定汇总 ===")
        bad = [r for r in results if r[2] not in ("MOVED_OK", "REPAINT_OK")]
        for (a, b, c) in results:
            L("  %-10s %-7s %s" % (a, b, c))
        L("PASS/FAIL: %s（%d/%d 条非 OK）" % ("FAIL" if bad else "PASS", len(bad), len(results)))
    log.close()
    return 1 if (bad or not results) else 0


if __name__ == "__main__":
    sys.exit(main())
