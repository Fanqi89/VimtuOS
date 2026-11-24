# -*- coding: utf-8 -*-
"""deskfolder64_test.py —— ★ v0.11.7 桌面 = 真实文件夹（<home>/Desktop）端到端验收。

用户需求原文（桌面图标真实文件.txt）：
  "桌面必须直接对应底层文件系统里的一个真实文件夹 … 1) 桌面即文件夹 2) 拖拽即移动
   3) 实时刷新（终端里 touch 一个新文件，桌面必须能感知并显示出图标，图标按扩展名匹配）"

判据全部落在**串口打点 + 重启后的事实**上（不靠"看起来像"）：

  阶段 1  桌面绑定真目录
    [DESKDIR] init dir=/home/<u>/Desktop vol=<n> exists=1 entries=<n>
    [DESKDIR] selftest PASS mask=63                     （路径拼接 / 唯一名 / 十进制 6 条纯逻辑判据）
    [DESK64] links load why=init … path=/etc/desktop.links（位置缓存；既有格式不变）

  阶段 2  实时刷新（**不碰鼠标**：只在终端里建文件）
    [DESKDIR] scan why=fs dir=… added=1 names=…,live.txt
      ★ 这一行只能由 vfs64 变更世代号触发 —— 说明"终端里写文件 -> 桌面自己刷新"是真链路，
        不是"某个 GUI 动作顺带刷了一下"。
    [DESK64] item … name=live.txt … glyph=1             （.txt -> 文本图标档位）

  阶段 3  拖到桌面 = 真移动（不是登记快捷方式）
    [DESKDIR] move in why=explorer-drag from=/<src> to=<desk>/<src> vol=… via=rename rc=0 ok=1
    [DESKDIR] scan … names=…,<src>                       （目录里真的多了一个条目）
    [DESK64] file add idx=… kind=9 … path=<desk>/<src>   （桌面项由目录条目生成）

  阶段 4  桌面删除 = 真进回收站（两套回收站合并成一套）
    [DESKDIR] trash name=<src> dir=<desk> rc=0 entry=<entry>
    [TRASH64] put name=<entry> orig=<desk>/<src> type=file …
    [DESKDIR] scan … removed=1                           （桌面图标从目录条目里消失）

  阶段 5  重启（同一块盘）
    [DESKDIR] init … entries>=1 + [DESKDIR] scan why=init … names=…  （重启后桌面仍然是那个真实目录）

  阶段 6  （第二个会话里，桌面上只有一个窗口，像素采样才干净）
    双击桌面回收站 -> [RECYCLE64] files why=open n>=1 virt=0（行来自 /Recycle，不是内存集合）
    [RECYCLE64] restore … real=1 rc=0 + [DESKDIR] scan why=restore … added=1（真搬回原位置）
    **窗口圆角像素判据**（deco64_test 只验几何，这里验像素）：
      窗口左上/右上角点 ≈ 窗口外侧壁纸（90° 弧把角切掉）
      弧以内（+2,+14）是窗口像素（弧不是空的）
      左下/右下角点是窗口像素（底边全宽直线没被啃）
      —— 三条同时成立，才证明 v0.11.6 修的"端帽 / Type-C 断层"真的没了

  阶段 7  不得出现 PANIC / TRIPLE FAULT / selftest FAIL / OOM

为什么用真系统盘（build64/sysdisk.img 的**副本**）：小系统盘夹具是刚格式化的空卷，
根目录没有条目可拖；副本保证共享产物不被写脏（v0.9.24 的教训）。

运行：py -3 tests/deskfolder64_test.py [--qemu <path>] [--keep]
"""

import argparse
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  find_qemu
import explorer64_test as ex       # noqa: E402  鼠标闭环
import dragdrop64_test as dd       # noqa: E402  Vm / Mon / wait_re / login / drag_from_to

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")


def open_terminal(vm, mon):
    """开始菜单数字快捷键 1 = 终端（startmenu64 的 kDigitApp[0] = APP_ID_TERM）。"""
    since = len(vm.log())
    mon.key("meta_l", wait=1.0)
    mon.key("1", wait=2.5)
    return vm.wait_log("[APP] term opened", 25, since) or vm.wait_log("[TERM] ready", 25, since)


def desk_items(vm):
    """串口里最后一次已知的桌面项表：name -> (idx, kind, x, y, glyph)。"""
    out = {}
    for m in re.finditer(r"\[DESK64\] item idx=(\d+) kind=(\d+) name=(\S+) x=(\d+) y=(\d+) "
                         r"w=\d+ h=\d+(?: glyph=(\d+))?", vm.log()):
        out[m.group(3)] = (int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(5)),
                           int(m.group(6)) if m.group(6) else -1)
    return out


def click_at(mon, x, y):
    goto_abs(mon, x, y)
    ex.single_click_at(mon)


def type_cmd(mon, text, per_key=0.09):
    """敲一行命令 + 回车。
    ★ 坑（实测踩到）：QEMU 的 `sendkey D` 不认大写 -> 字符被吞（`/home/vimtu/Desktop` 变成
      `/home/vimtu/esktop`，于是 touch 失败）。所以大写必须显式映射成 `shift-<小写>`。"""
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
             ">": "shift-dot", "<": "shift-comma", "=": "equal", ":": "shift-semicolon",
             "|": "shift-backslash", "*": "shift-8"}
    for ch in text:
        if ch in names:
            mon.key(names[ch], wait=per_key)
        elif ch.isdigit():
            mon.key(ch, wait=per_key)
        elif ch.isalpha():
            mon.key(ch.lower() if ch.isupper() else ch, wait=per_key) if not ch.isupper() \
                else mon.key("shift-" + ch.lower(), wait=per_key)
        else:
            raise ValueError("sendkey 不支持该字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.25)


def goto_abs(mon, x, y):
    """绝对定位（先归零再走过去）。
    ★ 坑：ex.park_cursor 默认 58 次 × 20px = 1160 < 屏幕宽 1280 —— 从右侧出发时归不到 0，
      残留 ~100px 偏移，之后所有 blind_goto 全偏（桌面图标 48px，直接点不中）。"""
    ex.park_cursor(mon, times=90)
    if x > 0:
        ex.move_px(mon, 0, +1, x)
    if y > 0:
        ex.move_px(mon, 1, +1, y)


def desk_icon_hit(vm, mon, item, tries=6):
    """把光标锁到某个桌面图标上（闭环：看 [UI] desktop icon select kind= 回执）。
    item = (idx, kind, x, y, glyph)；返回 True = 光标此刻确实压在那一项上。"""
    bx, by = item[2] + 24, item[3] + 24
    for i in range(tries):
        ox, oy = (0, -9, 9, -18, 18, 0)[i % 6], (0, 5, -5, 0, 0, -9)[i % 6]
        goto_abs(mon, bx + ox, by + oy)
        n = len(vm.log())
        ex.single_click_at(mon)
        sel = re.findall(r"\[UI\] desktop icon select kind=(\d+)", vm.log()[n:])
        if sel and int(sel[-1]) == item[1]:
            return True
    return False


def desk_drag_to(vm, mon, item, dst, tries=6):
    """把桌面图标 item 拖到 dst=(x,y)（先闭环锁定图标，再按 -> 分轴移动 -> 松）。"""
    if not desk_icon_hit(vm, mon, item, tries):
        return False
    n = len(vm.log())
    mon.send("mouse_button 1", wait=0.45)                 # 按下（此时光标就在图标上）
    dx = dst[0] - (item[2] + 24)
    dy = dst[1] - (item[3] + 24)
    if dx:
        ex.move_px(mon, 0, 1 if dx > 0 else -1, abs(dx))
    if dy:
        ex.move_px(mon, 1, 1 if dy > 0 else -1, abs(dy))
    time.sleep(0.6)
    mon.send("mouse_button 0", wait=1.0)                  # 松手
    return True


def double_click_at(mon, x, y):
    """桌面图标双击：必须用 ex.double_click（5 个事件、间隔 0.1s）——
    ex.single_click_at 会自己 sleep 0.55s，两次点之间 > 500ms，GUI 不认双击。"""
    ex.blind_goto(mon, x, y)
    ex.double_click(mon)


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_deskfolder_")
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(SYSDISK, disk)              # ★ 副本：绝不写脏共享产物
    s1 = os.path.join(tmp, "serial1.log")
    s2 = os.path.join(tmp, "serial2.log")
    checks = []
    state = {"ok": True}

    def check(name, cond, detail=""):
        state["ok"] = state["ok"] and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, log):
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:"):
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    vm = mon = None
    name_src = ""
    deskdir = ""
    try:
        # ==================== 阶段 1：启动 -> 桌面绑定真目录 ====================
        print("=== 阶段 1：真系统盘（副本）启动 -> 登录 -> 桌面 = 真实文件夹 ===")
        vm = dd.Vm(qemu, disk, s1, "Vimtu64-deskfolder")
        mon = dd.Mon(vm.port)
        check("锁屏 -> 登录 -> 桌面（[GUI64] ready）", dd.login(vm, mon), "serial=%s" % s1)

        m = dd.wait_re(vm, r"\[DESKDIR\] init dir=(/\S+) vol=(\d+) exists=(\d+) entries=(\d+)", 30)
        check("桌面绑定真实目录（[DESKDIR] init dir=… exists=1）",
              m is not None and m.group(3) == "1", m.group(0) if m else "（缺行）")
        deskdir = m.group(1) if m else ""
        check("桌面目录是 <home>/Desktop（判据：以 /Desktop 结尾）",
              deskdir.endswith("/Desktop"), "dir=%s" % deskdir)
        check("[DESKDIR] selftest PASS（路径拼接/唯一名/十进制 6 条）",
              "[DESKDIR] selftest PASS mask=63" in vm.log())
        check("位置缓存仍是 /etc/desktop.links（[DESK64] links load why=init … path=…）",
              dd.wait_re(vm, r"\[DESK64\] links load why=init \S+ .*path=/etc/desktop\.links", 20) is not None)
        forbid("阶段 1 日志", vm.log())
        if not state["ok"]:
            raise RuntimeError("阶段 1 失败（serial=%s）" % s1)

        # ==================== 阶段 2：终端里 touch -> 桌面自己刷新 ====================
        print("=== 阶段 2：终端 touch %s/live.txt -> 桌面实时出图标（不碰鼠标）===" % deskdir)
        check("终端能打开（开始菜单 1 = 终端）", open_terminal(vm, mon))
        time.sleep(1.0)
        n0 = len(vm.log())
        type_cmd(mon, "touch %s/live.txt" % deskdir)
        m_scan = dd.wait_re(vm, r"\[DESKDIR\] scan why=fs dir=(\S+) entries=(\d+) total=\d+ added=(\d+) "
                                r"removed=(\d+) retyped=\d+ hidden=\d+ slot=\d+ names=(\S*)", 25, n0)
        check("世代号触发目录重列（[DESKDIR] scan why=fs … added=1）",
              m_scan is not None and m_scan.group(3) == "1", m_scan.group(0)[:120] if m_scan else "（缺行）")
        check("新条目就在列表里（names 含 live.txt）",
              bool(m_scan) and "live.txt" in m_scan.group(5), m_scan.group(5) if m_scan else "-")
        time.sleep(1.0)
        it = desk_items(vm)
        check("桌面项出现 live.txt（[DESK64] item … name=live.txt）", "live.txt" in it, str(it.get("live.txt")))
        check("图标按扩展名匹配（.txt -> glyph=1 文本档位）",
              "live.txt" in it and it["live.txt"][4] == 1,
              "glyph=%s" % (it["live.txt"][4] if "live.txt" in it else "-"))

        # 再建一个**目录**和一个 .png：验证 0 / 2 档位（判据是串口的 glyph=，不看像素）
        n1 = len(vm.log())
        type_cmd(mon, "mkdir %s/docs2" % deskdir)
        time.sleep(1.2)
        type_cmd(mon, "echo hi > %s/pic.png" % deskdir)
        m_scan2 = dd.wait_re(vm, r"\[DESKDIR\] scan why=fs \S+", 25, n1)
        check("连续两次终端写操作都被感知（[DESKDIR] scan why=fs 再出现）", m_scan2 is not None,
              m_scan2.group(0)[:100] if m_scan2 else "（缺行）")
        time.sleep(1.5)
        it2 = desk_items(vm)
        check("目录条目 -> glyph=0 文件夹档位",
              "docs2" in it2 and it2["docs2"][4] == 0,
              "glyph=%s" % (it2["docs2"][4] if "docs2" in it2 else "-"))
        check("名字带 .png 的条目 -> glyph=2 图片档位",
              "pic.png" in it2 and it2["pic.png"][4] == 2,
              "glyph=%s" % (it2["pic.png"][4] if "pic.png" in it2 else "-"))
        forbid("阶段 2 日志", vm.log())

        # ==================== 阶段 3：拖到桌面 = 真移动 ====================
        # 拖拽源必须是**登录用户自己的文件**（这里 /home/vimtu/move_me.txt）：/etc 这种 root 目录
        # 被权限拒绝是产品的正确行为（实测 rc=-1 why=rename-failed），不该拿来当用例。
        print("=== 阶段 3：/home/vimtu/move_me.txt -> 桌面（真移动 rename）===")
        type_cmd(mon, "touch /home/vimtu/move_me.txt")
        time.sleep(1.0)
        check("资源管理器能打开（开始菜单 2 = 我的电脑）", dd.open_explorer(vm, mon))
        ex.SAFE_POINT = (ex.sx(ex.CONTENT_X + ex.CONTENT_W - 40), ex.sy(ex.CONTENT_Y + ex.CONTENT_H - 40))
        top_ok, top_det = ex.ensure_window_on_top(vm, mon)
        check("资源管理器在最前且能收点击", top_ok, top_det)
        dxe, dye, loops = ex.calibrate_offset(vm, mon)
        check("鼠标落点偏差已量出（闭环修正用）", True, "dx=%d dy=%d loops=%d" % (dxe, dye, loops))

        def last_block():
            """最近一次目录列举的条目表（内核先逐条打 item 行、最后打 nav 行）。"""
            lines = vm.log().splitlines()
            last = -1
            for i, ln in enumerate(lines):
                if ln.startswith("[UI] explorer item idx="):
                    last = i
            if last < 0:
                return {}
            j = last
            while j - 1 >= 0 and lines[j - 1].startswith("[UI] explorer item idx="):
                j -= 1
            out = {}
            for b in lines[j:last + 1]:
                m = re.search(r"\[UI\] explorer item idx=(\d+) name=(\S+) type=(\w+)", b)
                if m:
                    out[m.group(2)] = (int(m.group(1)), m.group(3))
            return out

        def open_dir(name, tries=3):
            """双击图标视图里的目录条目（进的目录名会出现在下一次列举里）。"""
            for _ in range(tries):
                blk = last_block()
                if name not in blk:
                    return False
                idx = blk[name][0]
                c = ex.icon_cell(idx)
                since_n = len(vm.log())
                ex.blind_goto(mon, c[0] + dxe, c[1] + dye)
                ex.double_click(mon)
                time.sleep(1.6)
                nb = last_block()
                if nb and name not in nb:
                    return "[UI] explorer enter" in vm.log()[since_n:]
            return False

        mc = re.search(r"\[UI\] explorer card idx=(\d+) letter=C: kind=browsable", vm.log())
        idx_c = int(mc.group(1)) if mc else 0
        cc = ex.card_center(idx_c)
        ex.aim_click(vm, mon, cc[0] + dxe, cc[1] + dye, "card:%d" % idx_c)
        got = dd.wait_re(vm, r"\[UI\] explorer enter letter=C: slot=\d+ ok items=(\d+)", 30)
        check("进入 C: 盘根（[UI] explorer enter … items=N）", got is not None,
              got.group(0) if got else "（缺行）")
        check("视图 = 图标", ex.ensure_view(vm, mon, "icons"))
        check("进 /home（双击目录条目 home）", open_dir("home"), str(list(last_block().keys())[:6]))
        check("进 /home/vimtu（双击目录条目 vimtu）", open_dir("vimtu"), str(list(last_block().keys())[:6]))
        blk = last_block()
        check("看到待拖的文件 move_me.txt", "move_me.txt" in blk, str(blk))

        if "move_me.txt" in blk:
            cell = ex.icon_cell(blk["move_me.txt"][0])
            src = (cell[0] + dxe, cell[1] + dye)
            since = len(vm.log())
            ex.blind_goto(mon, src[0], src[1])
            ex.single_click_at(mon)
            p = ex.last_probe(vm, since)
            check("按下点落在条目上（[UI] explorer click hit=item:<n>）",
                  p is not None and str(p[2]).startswith("item:"), str(p))

            dst = (ex.sx(ex.WIN_W + 320), ex.sy(40))     # 窗口右侧的桌面空白
            since = len(vm.log())
            dd.drag_from_to(mon, src, dst)
            m_begin = dd.wait_re(vm, r"\[UI\] explorer drag begin idx=\d+ name=(\S+)", 20, since)
            check("拖拽成立（drag begin）", m_begin is not None, m_begin.group(0) if m_begin else "（缺行）")
            name_src = m_begin.group(1) if m_begin else ""
            m_mv = dd.wait_re(vm, r"\[DESKDIR\] move in why=explorer-drag from=(/\S+) to=(\S+) vol=\d+ dir=\d+ "
                                  r"via=(rename|copy\+unlink) name=(\S+) rc=0 ok=1", 25, since)
            check("真移动成功（[DESKDIR] move in … via=rename rc=0 ok=1）", m_mv is not None,
                  m_mv.group(0)[:130] if m_mv else "（缺行）")
            check("落点就是 <桌面目录>/<名字>",
                  bool(m_mv) and bool(deskdir) and m_mv.group(2) == deskdir + "/" + m_mv.group(4),
                  "to=%s name=%s" % (m_mv.group(2) if m_mv else "-", m_mv.group(4) if m_mv else "-"))
            m_add = dd.wait_re(vm, r"\[DESK64\] file add idx=\d+ kind=9 dir=\d+ vol=\d+ path=(/\S*) ", 20, since)
            check("桌面项由目录条目生成（[DESK64] file add kind=9 path=<桌面>/<名字>）",
                  bool(m_add) and bool(deskdir) and m_add.group(1).startswith(deskdir + "/"),
                  m_add.group(1) if m_add else "（缺行）")
            time.sleep(1.2)
            it3 = desk_items(vm)
            check("桌面列表里真的有它（[DESK64] item 行）", bool(name_src) and name_src in it3,
                  "name=%s items=%s" % (name_src, ",".join(it3.keys())))
        else:
            it3 = desk_items(vm)
            check("阶段 3 前置：待拖文件可见", False, "block=%s" % str(blk))
        forbid("阶段 3 日志", vm.log())

        # ==================== 阶段 4：桌面删除 = 真进 /Recycle ====================
        print("=== 阶段 4：把桌面上的 %s 拖进回收站图标 -> 真进 /Recycle ===" % name_src)
        # ★ 先把窗口全部最小化（Ctrl+Shift+W）：桌面图标在屏幕左侧，被资源管理器盖住时
        #   按下去命中的是窗口而不是图标（实测：一条 [DESKDIR] trash 都不会出现）。
        since_m = len(vm.log())
        mon.key("ctrl-shift-w", wait=1.2)
        check("最小化所有窗口（[UI] hotkey ctrl+shift+w minimize）",
              vm.wait_log("[UI] hotkey ctrl+shift+w", 10, since_m), "")
        time.sleep(0.8)
        rec = it3.get("回收站")
        check("桌面回收站图标在（kind=1）", rec is not None and rec[1] == 1, str(rec))
        # 选"最左边的文件图标"当拖拽源（鼠标定位在左侧最稳，见文件头注释）
        files_only = {k: v for k, v in it3.items() if v[1] == 9}
        pick = min(files_only.items(), key=lambda kv: (kv[1][2], kv[1][3])) if files_only else None
        if pick is not None:
            name_src = pick[0]
            check("拖拽源 = 最左边的文件图标（%s @ %d,%d）" % (name_src, pick[1][2], pick[1][3]), True,
                  "所有文件项=%s" % str(files_only))
        if rec is not None and pick is not None:
            rx, ry = rec[2] + 24, rec[3] + 24                              # 回收站图标中心
            since = len(vm.log())
            check("光标能锁到该文件图标上（闭环：select kind=9）",
                  desk_drag_to(vm, mon, it3[name_src], (rx, ry)), "item=%s" % (it3[name_src],))
            m_tr = dd.wait_re(vm, r"\[DESKDIR\] trash why=desktop-icon name=(\S+) dir=(\S+) rc=0 entry=(\S+)", 25, since)
            check("真搬进回收站（[DESKDIR] trash … rc=0 entry=…）", m_tr is not None,
                  m_tr.group(0)[:130] if m_tr else "（缺行）")
            m_put = dd.wait_re(vm, r"\[TRASH64\] put name=(\S+) orig=(\S+) type=file vol=\d+ rc=0", 20, since)
            check("回收站里记录原位置（[TRASH64] put orig=<桌面>/<名字>）",
                  bool(m_put) and bool(deskdir) and m_put.group(2) == deskdir + "/" + name_src,
                  m_put.group(0) if m_put else "（缺行）")
            m_rm = dd.wait_re(vm, r"\[DESKDIR\] scan why=recycle \S+ added=\d+ removed=(\d+)", 20, since)
            check("桌面条目消失（[DESKDIR] scan … removed=1）",
                  m_rm is not None and m_rm.group(1) == "1", m_rm.group(0)[:120] if m_rm else "（缺行）")
        else:
            check("阶段 4 前置：桌面图标坐标可用", False, "items=%s" % str(it3))
        forbid("阶段 4 日志", vm.log())

        # ==================== 阶段 5：重启后仍然是那个真实目录 ====================
        print("=== 阶段 5：重启（同一块盘）-> 桌面目录内容还在 ===")
        vm.close()
        vm = dd.Vm(qemu, disk, s2, "Vimtu64-deskfolder-2")
        mon = dd.Mon(vm.port)
        check("第二次启动 -> 桌面就绪", dd.login(vm, mon), "serial=%s" % s2)
        m2 = dd.wait_re(vm, r"\[DESKDIR\] init dir=(\S+) vol=\d+ exists=1 entries=(\d+)", 30)
        check("重启后绑定的还是同一个目录",
              bool(m2) and bool(deskdir) and m2.group(1) == deskdir,
              m2.group(0) if m2 else "（缺行）")
        m3 = dd.wait_re(vm, r"\[DESKDIR\] scan why=init .*names=(\S*)", 20)
        check("重启后目录里仍有恢复回来的条目（names 含 %s）" % (name_src or "-"),
              bool(m3) and bool(name_src) and name_src in m3.group(1),
              m3.group(1) if m3 else "（缺行）")
        check("位置缓存读回（[DESK64] links load why=init … n>=1）",
              dd.wait_re(vm, r"\[DESK64\] links load why=init \S+ n=(\d+)", 20) is not None)
        forbid("阶段 5 日志", vm.log())

        # ==================== 阶段 6：桌面回收站 = 同一个 /Recycle（恢复）+ 圆角像素判据 ====================
        # 为什么放在重启后的**新会话**里：这时桌面上只有回收站一个窗口，"窗口外"的采样点才是干净壁纸，
        #   像素判据（下两角是窗口、上两角是壁纸）不会被别的窗口污染。
        print("=== 阶段 6：双击桌面回收站 -> 行来自 /Recycle -> 恢复 -> 圆角像素判据 ===")
        items = desk_items(vm)
        rec2 = items.get("回收站")
        check("桌面回收站图标在（kind=1）", rec2 is not None and rec2[1] == 1, str(rec2))
        since = len(vm.log())
        if rec2 is not None:
            check("光标能锁到回收站图标上（闭环：select kind=1）",
                  desk_icon_hit(vm, mon, rec2), "item=%s" % (rec2,))
            ex.double_click(mon)
        m_f = dd.wait_re(vm, r"\[RECYCLE64\] files why=(?:open|focus) n=(\d+) virt=(\d+) vol=\d+", 25, since)
        check("回收站窗口的行来自 /Recycle（[RECYCLE64] files … n>=1 virt=0）",
              m_f is not None and int(m_f.group(1)) >= 1 and m_f.group(2) == "0",
              m_f.group(0) if m_f else "（缺行）")
        m_g = dd.wait_re(vm, r"\[RECYCLE64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) client=\d+x\d+ row_h=\d+", 20, since)
        check("回收站窗口几何打点（[RECYCLE64] geom …）", m_g is not None,
              m_g.group(0) if m_g else "（缺行）")
        m_btn = dd.wait_re(vm, r"\[RECYCLE64\] btn idx=0 name=restore x=(\d+) y=(\d+) w=(\d+) h=(\d+)", 20, since)
        check("恢复按钮几何打点（[RECYCLE64] btn idx=0 name=restore）", m_btn is not None,
              m_btn.group(0) if m_btn else "（缺行）")

        # ---- 6a. 圆角像素判据（**差分**：开窗 vs 最小化后）----
        # 为什么用差分而不是"和壁纸比颜色"：这套主题的标题带是玻璃叠在壁纸上，
        #   实测 corner=(240,243,248) / ref=(242,246,250) 只差 2~6 —— 颜色阈值分不开。
        #   差分与颜色无关，只看"这颗像素到底有没有被窗口画过"：
        #     ① 左上/右上**角点**：A ≈ B  -> 角点没落笔 = 90° 弧把角切掉了（不是方角）
        #     ② 弧以内 (+2,+14)：A ≠ B   -> 弧以内是窗口像素（弧不是空的）
        #     ③ 左下/右下**角点**：A ≠ B -> 底边是全宽直线（没被啃）
        shot_a = os.path.join(tmp, "deco_on.ppm")
        shot_b = os.path.join(tmp, "deco_off.ppm")
        got_a = mon.shot(shot_a, wait=2.5) if m_g is not None else False

        # ---- 6b. 恢复（真从 /Recycle 搬回原位置）----
        if m_btn is not None:
            bx = int(m_btn.group(1)) + int(m_btn.group(3)) // 2
            by = int(m_btn.group(2)) + int(m_btn.group(4)) // 2
            since2 = len(vm.log())
            click_at(mon, bx, by)
            m_rs = dd.wait_re(vm, r"\[RECYCLE64\] restore kind=\d+ name=(\S+) to=(\S+) rc=0 why=-\S* real=1",
                              25, since2)
            check("恢复成功（[RECYCLE64] restore … real=1 rc=0 —— 真从 /Recycle 搬回）",
                  m_rs is not None, m_rs.group(0)[:130] if m_rs else "（缺行）")
            m_bk = dd.wait_re(vm, r"\[DESKDIR\] scan why=restore \S+ added=(\d+)", 20, since2)
            check("恢复后桌面重新列出它（[DESKDIR] scan why=restore … added=1）",
                  m_bk is not None and m_bk.group(1) == "1", m_bk.group(0)[:120] if m_bk else "（缺行）")
        else:
            check("阶段 6 前置：恢复按钮几何可用", False, "（缺 [RECYCLE64] btn idx=0）")

        # 关掉窗口再截第二张（作为"没有窗口"的对照）
        mon.key("ctrl-shift-w", wait=1.5)
        time.sleep(1.0)
        got_b = mon.shot(shot_b, wait=2.5)
        if got_a and got_b and m_g is not None:
            time.sleep(0.4)
            wa, ha, pa = ex.read_ppm(shot_a)
            wb, hb, pb = ex.read_ppm(shot_b)
            if pa and pb and wa == wb and ha == hb:
                wx, wy, ww, wh = (int(m_g.group(1)), int(m_g.group(2)),
                                  int(m_g.group(3)), int(m_g.group(4)))

                def diff(px, py):
                    a = ex.sample(pa, wa, px, py)
                    b = ex.sample(pb, wb, px, py)
                    return max(abs(a[i] - b[i]) for i in range(3))

                d_tl = diff(wx, wy)
                d_tr = diff(wx + ww - 1, wy)
                d_in = diff(wx + 2, wy + 14)
                d_bl = diff(wx + 1, wy + wh - 2)
                d_br = diff(wx + ww - 2, wy + wh - 2)
                print("      差分（窗口 %d,%d %dx%d）：角点 tl=%d tr=%d / 弧内=%d / 下角 bl=%d br=%d"
                      % (wx, wy, ww, wh, d_tl, d_tr, d_in, d_bl, d_br))
                check("圆角：左上角点没被窗口落笔（差分≈0 -> 弧把角切掉）", d_tl <= 6, "diff=%d" % d_tl)
                check("圆角：右上角点没被窗口落笔（左右对称）", d_tr <= 6, "diff=%d" % d_tr)
                check("圆角：弧以内是窗口像素（差分大 -> 弧不是空的）", d_in >= 12, "diff=%d" % d_in)
                check("圆角：左下角点是窗口像素（差分大 -> 底边全宽直线）", d_bl >= 12, "diff=%d" % d_bl)
                check("圆角：右下角点是窗口像素（左右对称）", d_br >= 12, "diff=%d" % d_br)
            else:
                check("圆角像素判据：两张截图同尺寸", False, "%s vs %s" % ((wa, ha), (wb, hb)))
        else:
            check("圆角像素判据（截图）", False, "shot_a=%s shot_b=%s" % (got_a, got_b))
        forbid("阶段 6 日志", vm.log())


    except Exception as e:      # 夹具/断言异常都要留下现场
        state["ok"] = False
        print("  [FAIL] 运行异常：%r" % (e,))
    finally:
        if vm is not None:
            vm.close()

    n_pass = sum(1 for _, ok in checks if ok)
    print("\n===== 汇总：%d/%d PASS =====" % (n_pass, len(checks)))
    for nm, ok in checks:
        if not ok:
            print("  FAIL: %s" % nm)
    if args.keep:
        print("现场保留：%s" % tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0 if state["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
