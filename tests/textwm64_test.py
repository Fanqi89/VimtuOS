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

# 开窗走**开始菜单数字快捷键**（kernel/startmenu64.cpp 的 kDigitApp）：
#   1 终端 / 2 我的电脑(资源管理器) / 3 系统监视器 / 4 计算器 / 5 扫雷 / 6 设置 / 7 任务管理器 / 8 关于
# Dock 下标（gui64.cpp kDockItems）保留在注释里备查：0 开始 / 1 我的电脑 / 3 终端 / 6 设置 / 7 任务管理器
# ★ 为什么换（v0.4.6）：Dock 路径是**真鼠标闭环**（宿主 rel 事件换算客人 px，单包上限 24），
#   开窗这一步本身就跟内容重排无关，却因为点不中 Dock 让 4/4 个应用全部 OPEN_FAIL。
APPS = {
    "explorer": {"digit": 2, "dock": 1, "title": "文件资源管理器", "app": 7, "layout_tag": None},
    "terminal": {"digit": 1, "dock": 3, "title": "终端", "app": 3, "layout_tag": r"\[UI\] term layout client="},
    "settings": {"digit": 6, "dock": 6, "title": "设置", "app": 5, "layout_tag": r"\[(SET64|UI) settings? layout"},
    "taskmgr":  {"digit": 7, "dock": 7, "title": "任务管理器", "app": 4, "layout_tag": r"\[UI\] tmgr layout"},
}


def pointer_stuck(traj):
    """拖拽轨迹里**客人光标位置从头到尾没变过** = 宿主鼠标注入没生效。

    实测（v0.4.6）：VMware 窗口没在前台/没抓到鼠标时，HostMouse.rel 的 mouse_event 全部落空，
    客人 cursor_pos 恒为同一点，自适应比例 |Δguest|/|Δhost| → 0 被夹到下限 0.05，
    步长按 1/0.05 放大又被单事件上限削掉 ⇒ 20s 预算内走不动 1px。
    这不是"窗口内容不随拖拽变化"（①②要抓的缺陷），是**环境缺前置条件**，必须与 DRAG_FAIL 分开报，
    否则真缺陷会被环境噪声淹没。
    """
    # ★ 轨迹元组形态：HostMouse.move_to 是 (i, pos, (dx,dy)) 3 元组，
    #   并把 ("nocursor", None, None) 这种哨兵条目也塞进来。历史上这里按 4 元组
    #   (i, pos, (dx,dy), ratio) 解包，HostMouse 改过之后就地炸掉（ValueError）。
    #   修法只改取值方式，语义不变：只看"客人光标位置"这一列。
    ps = [t[1] for t in traj
          if isinstance(t, tuple) and len(t) >= 2 and isinstance(t[1], tuple)]
    return len(ps) >= 2 and len(set(ps)) == 1


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



def band_stats(frame, x, y, w, h):
    """区域里的众数颜色 (r,g,b) 与其占比（"一片纯色"判定用）。在 rgb 上算，与显示源解耦。"""
    import numpy as np
    a = frame.rgb[max(0, y):y + h, max(0, x):x + w].reshape(-1, 3).astype(np.int32)
    if a.shape[0] == 0:
        return (0, 0, 0), 0.0
    v = (a[:, 0] << 16) | (a[:, 1] << 8) | a[:, 2]
    vals, cnts = np.unique(v, return_counts=True)
    big = int(vals[int(np.argmax(cnts))])
    return ((big >> 16) & 255, (big >> 8) & 255, big & 255), float(cnts.max()) / float(v.size)


def area_features(frame, x, y, w, h, ox=0, oy=0):
    """整窗像素特征（(ox,oy) 是相对取样偏移）。

    返回 (mean_abs_wrt_ref, ink, n)：mean_abs_wrt_ref 由调用方给参考帧算，这里只出 ink/n。
    为什么要有它：终端是**黑底 + 字缝**，9 宫格探针很容易整块落在空白上，
    位移前后都是纯黑 -> MAD 恒 0，会把"内容确实跟着走"误判成 CONTENT_STUCK（实测）。
    整窗统计把每一行字都算进去，字缝不再是盲区。
    """
    import numpy as np
    a = frame.rgb[max(0, y + oy):y + oy + h, max(0, x + ox):x + ox + w]
    if a.size == 0:
        return 0, 0
    lum = (a[:, :, 0].astype(np.int32) * 299 + a[:, :, 1].astype(np.int32) * 587
           + a[:, :, 2].astype(np.int32) * 114) // 1000
    ink = int((lum > 96).sum())          # 亮像素数（黑底上的字/边框）
    return ink, int(lum.size)


def current_win(s, title_part):
    """当前窗口几何：在 `[UI] win geom` 与 `[UI] win move end` 里取**日志中较晚**的一条。

    ★ 为什么必须这样：`[UI] win geom` 只在 new/set/fit 打（窗口被鼠标拖走后**不会**更新），
      原来 resize 用 s.win_geom() 取抓取点，拿到的是"创建时"位置 —— 窗口早被拖到别处了，
      于是每次都在空白桌面上按下 -> 串口里连 `[UI] win resize begin` 都没有，
      表现成 NO_RESIZE（看起来像"缩放没实现"，其实是测试点错地方）。
    """
    log = s.log()
    pats = [
        (r"\[UI\] win geom tag=\S+ title=.*? app=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) "
         r"client=(\d+)x(\d+)", None),
        (r"\[UI\] win move end x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) client=(\d+)x(\d+)", "move"),
    ]
    best = None
    for pat, kind in pats:
        for m in re.finditer(pat, log):
            if kind is None:
                g = m.groups()
                if title_part.lower() not in log[m.start():log.find("\n", m.start())].lower():
                    continue
                item = {"tag": "geom", "title": title_part, "app": int(g[0]),
                        "x": int(g[1]), "y": int(g[2]), "w": int(g[3]), "h": int(g[4]),
                        "client_w": int(g[5]), "client_h": int(g[6])}
            else:
                g = m.groups()
                item = {"tag": "move-end", "title": title_part, "app": -1,
                        "x": int(g[0]), "y": int(g[1]), "w": int(g[2]), "h": int(g[3]),
                        "client_w": int(g[4]), "client_h": int(g[5])}
            if best is None or m.start() > best[0]:
                best = (m.start(), item)
    return best[1] if best else None


def move_end_geom(s, mark=0):
    """从 `[UI] win move end x= y= w= h= client=WxH` 取拖动后真实几何（v0.10.8 新增打点）。

    ★ 为什么要它：像素判定分不清"窗口没搬走"和"搬走了但内容留在原地"——
      两者都表现为旧位置像素不变（stuck_mad≈0）。有了这条打点，几何是**权威**的：
      先证明窗口真的按拖拽搬到了哪里，再判内容有没有跟着走（判据比原来更硬）。
    """
    last = None
    for m in re.finditer(r"\[UI\] win move end x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) "
                         r"client=(\d+)x(\d+)", s.log()[mark:]):
        last = m
    if not last:
        return None
    g = [int(v) for v in last.groups()]
    return {"x": g[0], "y": g[1], "w": g[2], "h": g[3], "client_w": g[4], "client_h": g[5]}


def move_test(s, name, rep, f_before):
    """拖标题栏移动 -> 几何证明 + 像素判定。

    几何：`[UI] win move end` 必须出现，且窗口真的搬走了（否则窗口根本没动，谈不上"内容跟不跟"）。
    像素：moved = MAD(前帧[客户区原点+p], 后帧[客户区原点+实际位移+p]) = 内容跟着窗口走
          stuck = MAD(前帧[客户区原点+p], 后帧[同一屏幕位置+p])      = 内容留在原地
    """
    win = current_win(s, APPS[name]["title"])
    if not win:
        return "NO_WIN", {}
    scr_w, scr_h = s.vnc.w, s.vnc.h
    tx = 8 if win["x"] > scr_w // 2 else max(8, min(scr_w - win["w"] - 8, win["x"] + 240))
    ty = 12 if win["y"] > 200 else max(12, min(scr_h - 76 - win["h"] - 8, win["y"] + 150))
    dx, dy = tx - win["x"], ty - win["y"]
    if dx == 0 and dy == 0:
        dx = -180
    px, py = tv.title_drag_point(win)
    mark = s.mark()
    ok, tr = s.drag(px, py, px + dx, py + dy, steps=16)
    if not ok:
        # 环境缺前置条件（宿主鼠标注入没生效）与"拖拽了但内容不动"必须分开报
        return ("ENV_NO_POINTER" if pointer_stuck(tr) else "DRAG_FAIL"), {"traj": tr[-3:]}
    time.sleep(1.0)
    s.cap()                                   # 掉一帧
    f_after = s.cap("%s_move_r%d_after.png" % (name, rep))
    # ★ 截帧新鲜度守卫：宿主窗口抓帧偶尔拿到**上一帧**（WM 没重绘/PrintWindow 回旧位图），
    #   此时前后两帧逐像素一样 -> moved 与 stuck 同时≈0，会被误判成 "NO_MOVE(window 没动)"。
    #   （实测 terminal r3：几何 325->167 确实搬了，但像素差 0.98/0.64，就是这一条。）
    for _try in range(2):
        if f_before.mean_abs_diff(f_after, 0, 0, f_before.w, f_before.h) >= 0.5:
            break
        time.sleep(0.6)
        f_after = s.cap("%s_move_r%d_after_retry%d.png" % (name, rep, _try + 1))
    if f_before.mean_abs_diff(f_after, 0, 0, f_before.w, f_before.h) < 0.5:
        return "NO_VERDICT_PIXELS", {"why": "抓到的前后两帧逐像素相同（截帧过期，判不了）"}
    wa = move_end_geom(s, mark)
    nums = {"dx": dx, "dy": dy, "win_before": (win["x"], win["y"]), "win_expect": (tx, ty),
            "win_after": (wa["x"], wa["y"], wa["w"], wa["h"]) if wa else None,
            "cursor_traj_ok": True}
    if not wa:
        nums["why"] = "没有 [UI] win move end 打点（窗口拖动没进内核拖拽路径）"
        return "NO_MOVE_LOG", nums
    adx, ady = wa["x"] - win["x"], wa["y"] - win["y"]
    nums["actual_dxdy"] = (adx, ady)
    if abs(adx) < 8 and abs(ady) < 8:
        nums["why"] = "几何几乎没变（窗口没被拖走）"
        return "NO_MOVE(window 没动)", nums
    if max(abs(adx), abs(ady)) < 24:
        # 位移 <24px 时窗口内容与"留在原地"在像素上高度重叠，moved/stuck 没有分辨力
        # （实测：8px 位移下 stuck_mad 天生就小，判据两边都不成立）。如实报"未判定"。
        nums["why"] = "位移 <24px，像素判据无分辨力"
        return "NO_VERDICT_PIXELS", nums
    c0 = (win["x"] + 1, win["y"] + 25)
    pr = probes({"w": win["client_w"], "h": win["client_h"]})
    # ★ 只保留"前后两帧都完整落在画面内"的探针：窗口被拖到边缘时，
    #   后帧的采样框会跑到框外，mean_abs_diff 返回 999 哨兵值把均值污染成 338/999
    #   （旧版靠小位移侥幸没触发；窗口挪动 150~200px 后必现）。
    fw, fh = f_before.w, f_before.h

    def _in(x0, y0, p, ox, oy):
        x, y = x0 + ox, y0 + oy
        return 0 <= x and 0 <= y and x + p <= fw and y + p <= fh

    pr = [(ox, oy, p) for (ox, oy, p) in pr
          if _in(c0[0], c0[1], p, ox, oy) and _in(c0[0], c0[1], p, ox + adx, oy + ady)]
    nums["probes_used"] = len(pr)
    if len(pr) < 4:
        nums["why"] = "位移后落在画面内的探针不足（%d/9）" % len(pr)
        return "NO_VERDICT_PIXELS", nums
    moved = [f_before.mean_abs_diff(f_after, c0[0] + ox, c0[1] + oy, p, p, ox=adx, oy=ady)
             for (ox, oy, p) in pr]
    stuck = [f_before.mean_abs_diff(f_after, c0[0] + ox, c0[1] + oy, p, p)
             for (ox, oy, p) in pr]
    m = sum(moved) / len(moved)
    st = sum(stuck) / len(stuck)
    # ★ v0.10.8：再加一层**整窗**判据（9 宫格保留做诊断）。
    #   整窗 MAD 对"任何一块没跟着搬走"都敏感；ink 数（亮像素）用来证明"字还在"。
    cw, ch = win["client_w"], win["client_h"]
    area_moved = f_before.mean_abs_diff(f_after, c0[0], c0[1], cw, ch, ox=adx, oy=ady)
    area_stuck = f_before.mean_abs_diff(f_after, c0[0], c0[1], cw, ch)
    ink_a, npx = area_features(f_before, c0[0], c0[1], cw, ch)
    ink_b, _ = area_features(f_after, c0[0], c0[1], cw, ch, ox=adx, oy=ady)
    ink_ratio = (float(min(ink_a, ink_b)) / float(max(ink_a, ink_b, 1)))
    nums.update({"area_moved": round(area_moved, 2), "area_stuck": round(area_stuck, 2),
                 "ink_before": ink_a, "ink_after": ink_b, "ink_ratio": round(ink_ratio, 3),
                 "px": npx})
    nums.update({"moved_mad": round(m, 2), "stuck_mad": round(st, 2),
                 "moved_worst": round(max(moved), 2)})
    # 判据（v0.10.8，整窗为主；阈值比旧版 9 宫格更严，不是放松）：
    #   ① 整窗"跟着搬"：area_moved <= 4.0 且 area_stuck > 3*area_moved（旧位置明显变了）
    #   ② 字还在：ink_ratio >= 0.97（位移前后亮像素数一致，允许 3% 采样/抗锯齿误差）
    #   三条同时成立才是 MOVED_OK。
    if area_moved <= 4.0 and area_stuck > 3.0 * max(area_moved, 0.5) and ink_ratio >= 0.97:
        return "MOVED_OK", nums
    if area_stuck <= 1.5 and area_moved > 4.0:
        return "CONTENT_STUCK", nums
    if area_moved <= 4.0 and area_stuck <= 4.0:
        return "NO_MOVE(window 没动)", nums
    if ink_ratio < 0.90:
        return "CONTENT_LOST(字/像素没跟着走)", nums
    return "AMBIGUOUS", nums


def resize_end_geom(s):
    """从 `[UI] win resize end dir=.. x= y= w= h= client=WxH` 取**拖拽后**的窗口几何。

    ★ 修（v0.4.6，判据与事实不符）：原来用 s.win_geom() 取"最近一条 [UI] win geom"，
      但那条打点只在 **new/set/fit** 时打（gui64.cpp 的 log_win_geom64 调用点），
      用户用鼠标拖右下角**不打** win geom，只打 `[UI] win resize end`。于是拖完读到的还是
      创建时的旧几何 ⇒ 恒判 "几何没变 -> NO_RESIZE"。这里改读拖拽自己那条打点。
    """
    last = None
    for m in re.finditer(r"\[UI\] win resize end dir=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) "
                         r"client=(\d+)x(\d+)", s.log()):
        last = m
    if not last:
        return None
    g = [int(v) for v in last.groups()[1:]]
    return {"tag": "resize-end", "title": "", "app": -1, "x": g[0], "y": g[1],
            "w": g[2], "h": g[3], "client_w": g[4], "client_h": g[5]}


def resize_test(s, name, rep, f_before):
    """拖右下角缩放 -> 像素判定 + 串口重排打点。"""
    win = current_win(s, APPS[name]["title"])
    if not win:
        return "NO_WIN", {}
    scr_w, scr_h = s.vnc.w, s.vnc.h
    dw = 120 if win["x"] + win["w"] + 120 < scr_w else -120
    dh = 90 if win["y"] + win["h"] + 90 < scr_h - 76 else -90
    gx, gy = tv.resize_grip_point(win)
    mark = s.mark()
    ok, tr = s.drag(gx, gy, gx + dw, gy + dh, steps=18)
    if not ok:
        return ("ENV_NO_POINTER" if pointer_stuck(tr) else "DRAG_FAIL"), {"traj": tr[-3:]}
    time.sleep(1.2)
    s.cap()
    f_after = s.cap("%s_resize_r%d_after.png" % (name, rep))
    log_new = s.log()[mark:]
    # 拖拽后的真实几何：读 resize end 打点（拖拽不打 win geom，见 resize_end_geom 的说明）
    win2 = resize_end_geom(s) or current_win(s, APPS[name]["title"])
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
        col, frac = band_stats(f_after, bx, by, bw, bh)
        uni.append(frac)
        dom_colors.append(col)
    if not uni:
        nums["why"] = "带宽太小，没法判"
        return "AMBIGUOUS", nums
    nums["band_uniform"] = round(sum(uni) / len(uni), 4)
    nums["band_dominant_rgb"] = [str(v) for v in dom_colors]
    # ★ 判据：带的底色 == 外壳客户区底色 client_bg = rgb(240,240,240)（theme64.cpp）。
    #   历史上这里用 dominant_raw() + raw_to_rgb565() 反解 565 编码；但 v0.9.37 起
    #   显示判据统一走宿主窗口像素（WinView，已经是 RGB），raw 不是 565 了，
    #   dominant_raw() 返回的是**字节元组**，hex() 会直接抛 TypeError（跑到缩放才暴露）。
    #   改成在 rgb 上取众数颜色，口径不变、且与显示源解耦。
    bg_hits = [v == CLIENT_BG for v in dom_colors]
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
        # ★ 用户计划 ⑭：VMware 真屏（没有 QEMU fw_cfg -> 帧引擎必须**零打点**；同上零回归证据）
        _vsl = s.log()
        L("[env] 帧引擎打点=[VSYNC] 出现 %d 次（期望 0：VMware 无 fw_cfg 通道，功能默认关）" % _vsl.count("[VSYNC]"))
        if _vsl.count("[VSYNC]") != 0:
            results.append(("vsync-off", "log", "VSYNC_LINE_PRESENT"))
        if "PANIC" in _vsl:
            results.append(("panic", "log", "PANIC"))
        dg = s.dock_geom()
        L("[env] dock=%s" % dg)
        for name in names:
            info = APPS[name]
            # ★ 开窗走开始菜单数字快捷键（见 APPS 上面的说明）：不依赖鼠标闭环。
            #   原实现 s.open_dock_app(info["dock"], ...) 是点 Dock 图标中心。
            win = s.open_menu_app(info["digit"], info["title"])
            if not win:
                L("[%s] FAIL 开窗失败（开始菜单数字键 %d）" % (name, info["digit"]))
                results.append((name, "-", "OPEN_FAIL"))
                continue
            L("[%s] 开窗 ok title=%s x=%d y=%d w=%d h=%d client=%dx%d" %
              (name, win["title"], win["x"], win["y"], win["w"], win["h"],
               win["client_w"], win["client_h"]))
            time.sleep(0.8)
            # ★ 先给窗口腾出**可分辨**的位移：1024x768 下 966x660 的终端横向只能移 34px、
            #   纵向 8px，moved/stuck 两个 MAD 在小位移下没有分辨力（实测恒 AMBIGUOUS）。
            #   用右下角抓取点先把窗口缩小（缩放本身已由 [UI] win resize 证明可用），
            #   之后 move 才能拉出 ≥120px、resize 才有"变大"的余量。
            scr_w, scr_h = s.vnc.w, s.vnc.h
            if win["w"] > scr_w - 300 or win["h"] > scr_h - 230:
                gx, gy = tv.resize_grip_point(win)
                okz, _trz = s.drag(gx, gy, gx - 260, gy - 180, steps=1)
                time.sleep(1.0)
                w2 = current_win(s, info["title"])
                L("[%s] 先缩小窗口 ok=%s %s -> %s" %
                  (name, okz, (win["w"], win["h"]), w2 and (w2["w"], w2["h"])))
            for rep in range(1, args.reps + 1):
                for act in actions:
                    cur = current_win(s, info["title"])
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
            w4 = current_win(s, info["title"])
            if w4:
                cx, cy = tv.close_button(w4)
                s.click(cx, cy, settle=1.0)
                time.sleep(0.6)
                L("[%s] 关窗尝试后仍在? %s" % (name, bool(s.win_geom(info["title"]))))
        L("")
        L("=== 判定汇总 ===")
        # ENV_NO_POINTER = 宿主鼠标注入没生效（VMware 窗口没前台/没抓鼠标）→ 环境缺前置条件，
        # 与"拖了但内容不动"（CONTENT_STUCK / DRAG_FAIL）严格分开：后者仍然是 FAIL。
        env = [r for r in results if r[2] in ("ENV_NO_POINTER", "NO_VERDICT_PIXELS")]
        bad = [r for r in results
               if r[2] not in ("MOVED_OK", "REPAINT_OK", "ENV_NO_POINTER", "NO_VERDICT_PIXELS")]
        for (a, b, c) in results:
            L("  %-10s %-7s %s" % (a, b, c))
        if bad:
            L("PASS/FAIL: FAIL（%d/%d 条非 OK）" % (len(bad), len(results)))
        elif env:
            L("未验证：%d/%d 条因**环境**未就绪（宿主鼠标注入落空：客人光标零位移）—— "
              "开窗环节已过，拖拽①②本轮无结论" % (len(env), len(results)))
        else:
            L("PASS/FAIL: PASS（%d/%d 条 OK）" % (len(results), len(results)))
    log.close()
    if bad or not results:
        return 1
    return 2 if env else 0


if __name__ == "__main__":
    sys.exit(main())
