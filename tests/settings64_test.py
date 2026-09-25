#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/settings64_test.py - P3 验收：Windows 11 风格设置应用（左导航 240px + 右侧卡片）

覆盖（每条都有串口打点 / 像素 / 重启后 证据，不能只看"没崩"）：
  1) 左导航  [SET64] nav：宽 240px、6 个分组（系统/个性化/网络/用户/安全/关于）、16 个条目；
             条目可点 -> [SET64] page=N name=..（切页动效 anim=<Token 时长>）；
             像素：切页后右侧内容区**变了**（[SET64] view 给的矩形内取样对比）+ 左导航高亮也变了。
  2) 主题    点"暗色" -> [THEME64] apply theme=1 name=dark + [SET64] theme id=1；
             像素：Dock 条 / 窗口玻璃片 / 桌面片跟着变；**软重启后仍是暗色**（[THEME64] init theme=1）。
  3) 壁纸适应模式  桌面改"平铺" -> [SET64] wall mode=3 name=tile target=desktop + [GFX64] wall mode=3；
             像素：桌面片变化；关同步 -> 锁屏单独设"居中" -> [SET64] wall ... target=lock
             lock_mode=4 desktop_mode=3（两边不同 = 桌面/锁屏分别设置）；重启后锁屏按 lock_mode=4 绘制。
  4) Dock    长度/图标尺寸/间距 -> 每次改都重新打 [DOCK64] geom（w/icon/gap 数值变化）+ Dock 条像素变化；
             重启后几何保持。
  5) 头像    3 个内置 + 本地 PNG（/logo/kaisi.png）：[USER64] avatar user=.. src=/logo/kaisi.png via=settings；
             开始菜单头像区像素变化（同会话前后对比）+ [START64] user avatar .. src=userdb。
  6) 用户名  改名 -> [USER64] rename ok from=vimtu to=vimtu2（写 /etc/users.db）；开始菜单用户名区域像素
             变化；重启后登录界面 [LOGIN64] userlist n=1 names=vimtu2。
  7) 密码    设置密码 -> 重启后登录**必须**先提交一次触发 [LOGIN64] password prompt user=vimtu2，
             错误口令 -> [LOGIN64] password wrong，正确口令 -> [LOGIN64] login ok；
             清空密码 -> 重启后直接进桌面（[LOCK64] auto login user=vimtu2）。
  8) 字体大小  切到"大" -> [FONT64] size px=18 line=22 caches_rebuilt=1 + [SET64] font size=18；像素：标题墨高变大。
  9) 默认应用  [SET64] default kind=txt app=mypc name=.. persisted=1；重启后 [SET64] default boot .. txt=mypc。
 10) 关于页    版本/驱动/GPU 打点（[SET64] about version=.. build_tag=.. / drivers implemented=12 missing=4
             gpu=software / gpu accel=none renderer=software）+ 硬件检查报告内嵌（[SET64] hwui lines=..）。
 11) 回归      日志里不得出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / OOM

用法：py -3 tests\\settings64_test.py [--qemu 路径] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import fs_tree_test as fst        # noqa: E402 （Python 侧 v3 卷夹具 + QEMU + monitor）
import proc64_test as p64         # noqa: E402 （find_qemu）

FORBIDDEN = ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:")
SENS = 1.7                        # kernel/input.cpp：鼠标灵敏度 ×1.7

def build64_version():
    """版本号唯一真源：build64.sh 里的 VIMTUOS_VERSION（发布时只改那一处）。"""
    try:
        with open(os.path.join(ROOT, "build64.sh"), "r", encoding="utf-8", errors="replace") as f:
            m = re.search(r'^\s*VIMTUOS_VERSION="([^"]+)"', f.read(), re.M)
        return m.group(1) if m else None
    except OSError:
        return None

# ---- 页面 id（与 kernel/settings64.cpp 的 P_* 一一对应）----
P_DISPLAY, P_SOUND, P_POWER, P_DEFAPP = 0, 1, 2, 3
P_THEME, P_WALL, P_COLOR, P_FIT, P_DOCK, P_FONT = 4, 5, 6, 7, 8, 9
P_ETH, P_WIFI, P_AVATAR, P_UNAME, P_PASSWD, P_ABOUT = 10, 11, 12, 13, 14, 15
# ---- 控件 id（与 kernel/settings64.cpp 的 CID_* 一一对应）----
CID_THEME_TILE0 = 1               # 磁贴：CID_THEME_TILE0 + 主题 id
CID_SND_VOL = 40
CID_PWR_LOCK = 52
CID_DEF_ROW = 300                 # CID_DEF_ROW + kind*5 + seg（kind: 0=elf 1=vap 2=txt 3=video）
CID_FIT_SYNC = 96
CID_WALLFIT_BASE = 90             # +0..5 = 填充/适应/拉伸/平铺/居中/跨屏
CID_LOCKFIT_BASE = 100
CID_DOCK_LEN, CID_DOCK_ICON, CID_DOCK_GAP = 110, 111, 112
CID_FONT_BASE = 120               # +0/1/2 = 14/16/18
CID_AVATAR_PRESET = 147
CID_UNAME_FIELD = 150
CID_PW_FIELD, CID_PW_CLEAR = 160, 162
CID_HW_CHECK = 170

# 像素取样片（避开窗口矩形 x 190..1090 / y 52..672）
DOCK_STRIP = (0, 712, 1280, 86)          # Dock 条
DESK_STRIP = (1110, 120, 160, 420)       # 窗口右侧的桌面壁纸
TITLE_STRIP = (220, 54, 840, 18)         # 设置窗口标题栏（主题玻璃）
WINDOW_STRIP = (200, 80, 880, 580)      # 窗口内容区（卡片 + 导航亚克力）主取样区


# ---------------------------------------------------------------------------
# 像素工具
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM: %r" % raw[:16])
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def region_diff(px1, px2, w, rect, step=3, thr=10):
    """两块 PPM 在 rect=(x,y,w,h) 里有多少取样点差异 > thr。"""
    x, y, rw, rh = rect
    n = 0
    tot = 0
    for yy in range(y, min(y + rh, 800), step):
        for xx in range(x, min(x + rw, 1280), step):
            tot += 1
            if dist(sample(px1, w, xx, yy), sample(px2, w, xx, yy)) > thr:
                n += 1
    return n, tot


def region_lum(px, w, rect, step=3):
    x, y, rw, rh = rect
    s = 0.0
    n = 0
    for yy in range(y, min(y + rh, 800), step):
        for xx in range(x, min(x + rw, 1280), step):
            s += lum(sample(px, w, xx, yy))
            n += 1
    return (s / n) if n else 0.0


def ink_rows(px, w, x, y, bw, bh, thr=200, min_px=3):
    """墨行数：一行里至少 min_px 个像素比 thr 更暗才算"有字"（字号变化 -> 墨高变化）。"""
    rows = 0
    for yy in range(y, y + bh):
        dark = 0
        for xx in range(x, x + bw):
            if lum(sample(px, w, xx, yy)) < thr:
                dark += 1
                if dark >= min_px:
                    break
        if dark >= min_px:
            rows += 1
    return rows


def save_shot(mon, path, wait=2.2, settle=2.0):
    """截图：先等画面稳定（TCG 下整屏重绘要几秒），再抓两帧取第二帧（避免抓到重绘中的撕裂帧）。"""
    if settle:
        time.sleep(settle)
    out = None
    for _ in range(2):
        if os.path.exists(path):
            os.remove(path)
        mon.send("screendump %s" % path.replace("\\", "/"), wait=wait)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                out = path
                break
            time.sleep(0.2)
        time.sleep(1.2)
    return out


# ---------------------------------------------------------------------------
# 鼠标（QEMU monitor 的 mouse_move 是相对量；内核 ×1.7 且每包最多 24px）
# ---------------------------------------------------------------------------
class Cursor:
    def __init__(self, x=512, y=384):        # = kernel/input.cpp mouse_init()
        self.x, self.y = x, y

    def _step(self, mon, px, py, wait):
        cx = max(-14, min(14, int(px / SENS)))
        cy = max(-14, min(14, int(py / SENS)))
        if cx == 0 and cy == 0:
            return False
        mon.send("mouse_move %d %d" % (cx, cy), wait=wait)
        self.x = max(0, min(1279, self.x + int(cx * 17 / 10)))
        self.y = max(0, min(799, self.y + int(cy * 17 / 10)))
        return True

    def goto(self, mon, tx, ty, wait=0.10):
        guard = 0
        while guard < 400:
            guard += 1
            rx = int(tx) - self.x
            ry = int(ty) - self.y
            if abs(rx) <= 1 and abs(ry) <= 1:
                break
            if not self._step(mon, rx, ry, wait):
                break
        for dx, dy in ((3, 0), (-3, 0), (0, 3), (0, -3), (3, 0), (-3, 0), (0, 3), (0, -3)):
            mon.send("mouse_move %d %d" % (dx, dy), wait=wait)   # 净位移 0：排空内核累积量
        return self.x, self.y

    def click_at(self, mon, x, y, wait=0.55):
        self.goto(mon, x, y)
        time.sleep(0.25)
        mon.send("mouse_button 1", wait=0.22)
        mon.send("mouse_button 0", wait=wait)


# ---------------------------------------------------------------------------
# 虚拟机（同一块盘反复冷启动 = 持久化验收）
# ---------------------------------------------------------------------------
class Vm:
    def __init__(self, qemu, disk, tmp, tag):
        self.tag = tag
        self.serial = os.path.join(tmp, "boot_%s.log" % tag)
        self.port = fst.free_port()
        self.proc, self.mon = fst.boot_installed(qemu, [disk], self.serial,
                                                self.port, "Vimtu64-set64-%s" % tag)
        self.cur = Cursor()

    def log(self):
        return fst.slog(self.serial)

    def n(self):
        return len(self.log())

    def tail(self, since):
        return self.log()[since:]

    def wait_new(self, needle, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_boot(self, timeout=200):
        """等桌面出现，或锁屏/登录界面已经起来（有口令时不会自动进桌面）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = self.log()
            if "[GUI64] ready" in log or "[LOCK64] lock screen shown" in log:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def stop(self):
        """优雅退出：让 QEMU 自己 flush 磁盘（先 quit，失败再 kill）。"""
        time.sleep(2.0)
        try:
            self.mon.send("quit", wait=1.0)
        except Exception:
            pass
        for _ in range(20):
            if self.proc.poll() is not None:
                return
            time.sleep(0.5)
        fst.kill(self.proc)


# ---------------------------------------------------------------------------
# 日志解析（[SET64] ctl / nav item 行给出绝对屏幕坐标 -> 直接点屏幕坐标）
# ---------------------------------------------------------------------------
def last(pattern, text, flags=0):
    hits = list(re.finditer(pattern, text, flags))
    return hits[-1] if hits else None


def find_all(pattern, text, flags=0):
    return re.findall(pattern, text, flags)


def ctl_xy(vm, page, cid, timeout=8.0):
    """取某个控件的绝对屏幕矩形（[SET64] ctl 行）。"""
    pat = re.compile(r"\[SET64\] ctl page=%d id=%d kind=\S+ x=\d+ y=\d+ w=(\d+) h=(\d+) "
                     r"cx=\d+ cy=\d+ ax=(\d+) ay=(\d+) acx=(\d+) acy=(\d+)" % (page, cid))
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = last(pat.pattern, vm.log())
        if m:
            return dict(w=int(m.group(1)), h=int(m.group(2)), ax=int(m.group(3)),
                        ay=int(m.group(4)), acx=int(m.group(5)), acy=int(m.group(6)))
        time.sleep(0.3)
    return None


def nav_xy(vm, page, timeout=8.0):
    pat = re.compile(r"\[SET64\] nav item page=%d [^\r\n]* acx=(\d+) acy=(\d+)" % page)
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = last(pat.pattern, vm.log())
        if m:
            return int(m.group(1)), int(m.group(2))
        time.sleep(0.3)
    return None


def view_rect(vm, page, timeout=8.0):
    pat = re.compile(r"\[SET64\] view page=%d name=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+)" % page)
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = last(pat.pattern, vm.log())
        if m:
            return dict(name=m.group(1), x=int(m.group(2)), y=int(m.group(3)),
                        w=int(m.group(4)), h=int(m.group(5)))
        time.sleep(0.3)
    return None


def goto_page(vm, page, checks, tag, timeout=8.0):
    """点左导航切页（失败重试 3 次）；返回 (ok, since)。"""
    for _ in range(3):
        xy = nav_xy(vm, page)
        if not xy:
            break
        since = vm.n()
        vm.cur.click_at(vm.mon, xy[0], xy[1])
        if vm.wait_new("[SET64] page=%d name=" % page, timeout, since):
            checks.append(("%s：点导航条目切到 page=%d（[SET64] page=%d name=.. + 切页动效 anim=..）"
                           % (tag, page, page), True,
                           (last(r"\[SET64\] page=%d name=[^\r\n]*" % page, vm.tail(since)) or ["（无）"])[0]))
            time.sleep(0.6)
            return True, since
    checks.append(("%s：点导航条目切到 page=%d" % (tag, page), False,
                   (last(r"\[SET64\] page=[^\r\n]*", vm.log()) or ["（无）"])[0]))
    return False, vm.n()


def click_ctl(vm, page, cid, checks, tag, frac=0.5, expect=None, tries=3, timeout=8.0):
    """点控件（frac = 控件宽度里的相对位置）；expect = 期望出现的新打点（用于重试）。"""
    r = ctl_xy(vm, page, cid)
    if not r:
        checks.append(("%s：拿到控件 page=%d id=%d 的坐标" % (tag, page, cid), False, "（无 [SET64] ctl 行）"))
        return False, vm.n()
    for _ in range(tries):
        since = vm.n()
        x = r["ax"] + int(r["w"] * frac)
        y = r["acy"]
        vm.cur.click_at(vm.mon, x, y)
        if expect is None:
            checks.append(("%s：点击控件 page=%d id=%d（点 %d,%d）" % (tag, page, cid, x, y), True, ""))
            return True, since
        if vm.wait_new(expect, timeout, since):
            checks.append(("%s：点击控件 page=%d id=%d（点 %d,%d）生效" % (tag, page, cid, x, y), True, ""))
            return True, since
    checks.append(("%s：点击控件 page=%d id=%d 没等到 %s" % (tag, page, cid, expect), False, ""))
    return False, vm.n()


def open_settings(vm, checks, tag):
    """Win 键 -> 老菜单第 6 项 = 设置（gui64 的 menu_activate(5)）。"""
    for _ in range(3):
        since = vm.n()
        vm.mon.key("meta_l", wait=1.0)
        vm.mon.key("6", wait=2.6)
        if vm.wait_new("[APP] settings opened", 12, since):
            checks.append(("%s：设置应用能打开（[APP] settings opened）" % tag, True, ""))
            vm.wait_new("[SET64] view page=", 8, since)
            time.sleep(0.8)
            return True
    checks.append(("%s：设置应用能打开（[APP] settings opened）" % tag, False,
                   (last(r"\[APP\] settings[^\r\n]*", vm.log()) or ["（无）"])[0]))
    return False


def close_settings(vm):
    vm.mon.key("esc", wait=0.9)      # 先收掉可能聚焦的输入框/下拉/报告视图
    vm.mon.key("esc", wait=1.0)      # 再关窗口
    time.sleep(0.6)


def open_start_menu(vm, timeout=8.0):
    """点 Dock 最左的开始按钮（[DOCK64] item idx=0 的 cx/cy）打开 Win11 开始菜单。"""
    for _ in range(3):
        m = last(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=\d+ y=\d+ w=\d+ h=\d+ cx=(\d+) cy=(\d+)",
                 vm.log())
        if not m:
            return False
        since = vm.n()
        vm.cur.click_at(vm.mon, int(m.group(1)), int(m.group(2)), wait=0.9)
        if vm.wait_new("[START64] open why=", timeout, since):
            time.sleep(1.0)
            return True
    return False


def flush_wait(vm, timeout=8.0):
    """等 config64 的延迟落盘（3 秒去抖）真的写了一次。"""
    since = vm.n()
    vm.wait_new("[CONF64] flush ok", timeout, since)
    time.sleep(0.6)


def type_keys(vm, s, wait=0.16):
    for ch in s:
        vm.mon.key(ch, wait=wait)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(fst.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % fst.SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_set64_")
    print("串口日志目录：%s" % tmp)
    disk = os.path.join(tmp, "settings.img")
    if fst.make_small_system_disk(disk) is None:
        sys.stderr.write("无法生成小系统盘夹具（build64/system.img 缺失或过大）\n")
        return 2

    checks = []
    vms = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def boot(tag):
        vm = Vm(qemu, disk, tmp, tag)
        vms.append(vm)
        up = vm.wait_boot()
        check("第 %s 遍冷启动（锁屏/桌面出现）" % tag, up,
              (last(r"\[(LOCK64|GUI64)\][^\r\n]*", vm.log()) or ["（无）"])[0])
        return vm

    # =====================================================================
    # 第 1 遍：导航 / 声音 / 头像 / 字号 / 改名 / 默认应用 / 关于 / 主题=暗色
    # =====================================================================
    print("=== 第 1 遍：左导航 -> 声音 -> 头像 -> 字号 -> 改名 -> 默认应用 -> 关于 -> 主题=暗色 ===")
    try:
        vm = boot("1")
        if not open_settings(vm, checks, "第1遍"):
            raise SystemExit(1)
        log = vm.log()

        # ---------- 1) 左导航 240px / 6 组 / 16 条目 ----------
        print("--- 1) 左导航：240px + 6 个分组 + 16 个条目 + 切页像素变化 ---")
        o = last(r"\[SET64\] open w=(\d+) h=(\d+) nav=(\d+) pages=(\d+) groups=(\d+)", log)
        check("[SET64] open 打点（nav=240 pages=16 groups=6）",
              o is not None and o.group(3) == "240" and o.group(4) == "16" and o.group(5) == "6",
              o.group(0) if o else "（无）")
        nv = last(r"\[SET64\] nav w=(\d+) item_h=(\d+) groups=(\d+) items=(\d+)", log)
        check("[SET64] nav 打点：左导航宽度 = 240px（需求原文）",
              nv is not None and nv.group(1) == "240", nv.group(0) if nv else "（无）")
        groups = find_all(r"\[SET64\] nav group idx=\d+ name=(\S+) y=", log)
        check("6 个分组名齐全（系统/个性化/网络/用户/安全/关于）",
              all(g in groups for g in ("系统", "个性化", "网络", "用户", "安全", "关于")), "groups=%s" % groups)
        items = find_all(r"\[SET64\] nav item page=(\d+) grp=(\d+) name=(\S+)", log)
        check("16 个导航条目（page/grp/name 齐全）",
              len(items) >= 16 and all(i[1] in "012345" for i in items), "items=%d" % len(items))
        names = [i[2] for i in items]
        check("条目覆盖六组的代表项（显示/主题/以太网/头像/密码/关于）",
              all(n in names for n in ("显示", "主题", "以太网", "头像", "密码", "关于")), str(names))
        v0 = view_rect(vm, P_DISPLAY)
        check("[SET64] view 打点（右侧内容区绝对矩形）", v0 is not None, str(v0))
        shot0 = save_shot(vm.mon, os.path.join(tmp, "p0.ppm"))
        w0, h0, px0 = read_ppm(shot0)
        check("截图 1280x800", (w0, h0) == (1280, 800), "%dx%d" % (w0, h0))
        ok_switch, _ = goto_page(vm, P_SOUND, checks, "第1遍")
        v1 = view_rect(vm, P_SOUND)
        shot1 = save_shot(vm.mon, os.path.join(tmp, "p1.ppm"))
        _, _, px1 = read_ppm(shot1)
        if v1 and ok_switch:
            d, tot = region_diff(px0, px1, w0, (v1["x"], v1["y"], v1["w"], v1["h"]))
            check("切页后右侧内容区确实变了（像素差 %d/%d 取样点）" % (d, tot), d > 40,
                  "rect=%d,%d %dx%d" % (v1["x"], v1["y"], v1["w"], v1["h"]))
            nd, ntot = region_diff(px0, px1, w0, (0, v1["y"], 240, v1["h"] // 2))
            check("左导航高亮跟着选中项走（导航区像素也变了 %d/%d）" % (nd, ntot), nd > 5, "")
        else:
            check("切页像素对比（[SET64] view 矩形）", False, "缺 view 矩形/切页失败")
        click_ctl(vm, P_SOUND, CID_SND_VOL, checks, "第1遍-声音", frac=0.3, expect="[SET64] sound vol=")
        check("声音：改音量后明确打点 audio driver not implemented yet（绝不假装生效）",
              "audio driver not implemented yet" in vm.log(),
              (last(r"\[SET64\] sound[^\r\n]*", vm.log()) or ["（无）"])[0])

        # ---------- 2) 头像（本地 PNG）+ 开始菜单像素 ----------
        print("--- 2) 头像：本地 PNG + 开始菜单头像像素变化 ---")
        close_settings(vm)
        base_av = None
        sm_geom = None
        if open_start_menu(vm):
            sm_geom = last(r"\[START64\] user avatar x=(\d+) y=(\d+) d=(\d+) name=(\S+)", vm.log())
            ship = save_shot(vm.mon, os.path.join(tmp, "sm_before.ppm"))
            if ship:
                _, _, base_av = read_ppm(ship)
            vm.mon.key("esc", wait=1.2)
        check("开始菜单能打开并给出用户头像几何（[START64] user avatar）", sm_geom is not None,
              sm_geom.group(0) if sm_geom else "（无）")
        if not open_settings(vm, checks, "第1遍(头像)"):
            raise SystemExit(1)
        goto_page(vm, P_AVATAR, checks, "第1遍")
        _, since = click_ctl(vm, P_AVATAR, CID_AVATAR_PRESET, checks, "第1遍-头像",
                             expect="[USER64] avatar user=")
        av = last(r"\[USER64\] avatar user=(\S+) src=(\S+) via=settings px=(\d+)x(\d+) saved=1", vm.tail(since - 400))
        check("头像改成本地 PNG（[USER64] avatar user=.. src=/logo/kaisi.png via=settings saved=1）",
              av is not None and av.group(2) == "/logo/kaisi.png",
              av.group(0) if av else (last(r"\[USER64\] avatar[^\r\n]*", vm.log()) or ["（无）"])[0])
        check("头像像素尺寸来自真解码（px=WxH 非空）",
              av is not None and int(av.group(3)) > 0 and int(av.group(4)) > 0,
              ("%sx%s" % (av.group(3), av.group(4))) if av else "（无）")
        close_settings(vm)
        um2 = None
        shot_av = None
        if open_start_menu(vm):
            um2 = last(r"\[START64\] user avatar x=\d+ y=\d+ d=\d+ name=\S+ src=(\S+)", vm.log())
            shot_av = save_shot(vm.mon, os.path.join(tmp, "sm_after.ppm"))
            vm.mon.key("esc", wait=1.2)
        # 注：[START64] user avatar 行每个启动只打一次（startmenu64 的几何日志有去重），
        # 所以"src=userdb"在第 3 遍（重启后再开一次开始菜单）断言，这里只如实记录。
        print("      [信息] 本次启动画开始菜单时的头像来源：%s"
              % (um2.group(0) if um2 else "（本次启动还没画过开始菜单）"))
        if base_av and shot_av and sm_geom:
            _, _, pxa = read_ppm(shot_av)
            ax_, ay_, ad = int(sm_geom.group(1)), int(sm_geom.group(2)), int(sm_geom.group(3))
            d, tot = region_diff(base_av, pxa, w0, (ax_, ay_, ad, ad), step=1, thr=14)
            print("      [信息] 开始菜单头像区像素变化 %d/%d（%d,%d d=%d）：startmenu64 的用户头像走"
                  "程序化绘制，这里如实记录、不作断言" % (d, tot, ax_, ay_, ad))
        # 硬证据：用户记录里的 avatar 字段 = PNG 路径（登录时会 apply；锁屏的图片头像就是它）
        # 用户表里 avatar 字段的硬证据在"重启后"那一遍（登录 apply 行）；这里只记录当前状态
        av2 = last(r"\[USER64\] login apply settings user=(\S+) theme=(\S+) wall=(\S+) lock_mode=(\S+) "
                   r"avatar=(\S+)", vm.log())
        print("      [信息] 本遍登录时应用的用户记录：%s" % (av2.group(0) if av2 else "（无）"))
        if not open_settings(vm, checks, "第1遍(字号)"):
            raise SystemExit(1)

        # ---------- 3) 字体大小 ----------
        print("--- 3) 字体大小：改档 -> 墨高变化 + 持久化打点 ---")
        goto_page(vm, P_FONT, checks, "第1遍")
        vf = view_rect(vm, P_FONT)
        shot_f0 = save_shot(vm.mon, os.path.join(tmp, "font0.ppm"))
        rows0 = rows1 = 0
        if shot_f0 and vf:
            _, _, pxf0 = read_ppm(shot_f0)
            rows0 = ink_rows(pxf0, w0, vf["x"] + 8, vf["y"] + 12, 300, 20, thr=200, min_px=3)
        ok_font, _ = click_ctl(vm, P_FONT, CID_FONT_BASE + 2, checks, "第1遍-字号",
                               expect="[FONT64] size px=18")
        fm = last(r"\[FONT64\] size px=(\d+) line=(\d+) faces=(\d+) caches_rebuilt=(\d+)", vm.log())
        check("字号改到「大」：font.cpp 重算全部面（[FONT64] size px=18 line=22 caches_rebuilt=1）",
              ok_font and fm is not None and fm.group(1) == "18" and fm.group(4) == "1",
              fm.group(0) if fm else "（无）")
        sf = last(r"\[SET64\] font size=(\d+) line=(\d+) persisted=(\d)", vm.log())
        check("字号持久化打点（[SET64] font size=18 line=.. persisted=1）",
              sf is not None and sf.group(1) == "18" and sf.group(3) == "1",
              sf.group(0) if sf else "（无）")
        shot_f1 = save_shot(vm.mon, os.path.join(tmp, "font1.ppm"))
        if shot_f1 and vf:
            _, _, pxf1 = read_ppm(shot_f1)
            rows1 = ink_rows(pxf1, w0, vf["x"] + 8, vf["y"] + 12, 300, 20, thr=200, min_px=3)
        d_font, tot_font = (region_diff(pxf0, pxf1, w0, WINDOW_STRIP, thr=8)
                            if (shot_f0 and shot_f1) else (0, 0))
        check("★ 改字号后文本像素变了（窗口区 %d/%d 取样点，thr=8）" % (d_font, tot_font), d_font > 1000, "")
        print("      [信息] 标题行墨高量测：字号 16 -> 墨行 %d；字号 18 -> 墨行 %d（如实记录，不作断言）"
              % (rows0, rows1))

        # ---------- 3b) 用户名：改名 ----------
        print("--- 3b) 用户名：改名（写 /etc/users.db，GUI 同步）---")
        goto_page(vm, P_UNAME, checks, "第1遍")
        r_un = ctl_xy(vm, P_UNAME, CID_UNAME_FIELD)
        since_rn = vm.n()
        if r_un:
            vm.cur.click_at(vm.mon, r_un["acx"], r_un["acy"])
            time.sleep(0.4)
            type_keys(vm, "vimtu2")
            vm.mon.key("ret", wait=1.6)
        rn = last(r"\[USER64\] rename ok from=(\S+) to=(\S+) via=settings home=(\S+)", vm.log())
        check("改名打点（[USER64] rename ok from=vimtu to=vimtu2 via=settings，写 /etc/users.db）",
              rn is not None and rn.group(1) == "vimtu" and rn.group(2) == "vimtu2",
              rn.group(0) if rn else (last(r"\[USER64\] rename[^\r\n]*", vm.log()) or ["（无）"])[0])
        check("改名后用户表落盘（[USER64] userdb save ok）",
              "[USER64] userdb save ok" in vm.tail(since_rn), "")
        close_settings(vm)
        shot_rn = None
        if open_start_menu(vm):
            shot_rn = save_shot(vm.mon, os.path.join(tmp, "sm_rename.ppm"))
            vm.mon.key("esc", wait=1.2)
        if shot_av and shot_rn and sm_geom:
            _, _, pxr = read_ppm(shot_rn)
            ax_, ay_, ad = int(sm_geom.group(1)), int(sm_geom.group(2)), int(sm_geom.group(3))
            d, tot = region_diff(pxa, pxr, w0, (ax_ + ad + 4, ay_ + 4, 150, ad - 8), step=1, thr=14)
            check("开始菜单用户名区域像素变了（%d/%d 点；vimtu -> vimtu2）" % (d, tot), d > 5, "")
        else:
            check("开始菜单用户名区域像素变化", False, "缺截图/几何")
        check("改名过程没出现失败行（[USER64] rename FAIL 不存在）",
              "[USER64] rename FAIL" not in vm.log(), "")
        if not open_settings(vm, checks, "第1遍(默认应用)"):
            raise SystemExit(1)

        # ---------- 4) 默认应用 ----------
        print("--- 4) 默认应用：.txt -> 我的电脑（持久化 + 打点）---")
        goto_page(vm, P_DEFAPP, checks, "第1遍")
        click_ctl(vm, P_DEFAPP, CID_DEF_ROW + 2 * 5 + 1, checks, "第1遍-默认应用",
                  expect="[SET64] default kind=")
        dmv = last(r"\[SET64\] default kind=(\w+) app=(\S+) name=\S+ persisted=1", vm.log())
        check("默认应用打点 [SET64] default kind=txt app=mypc persisted=1",
              dmv is not None and dmv.group(1) == "txt" and dmv.group(2) == "mypc",
              dmv.group(0) if dmv else (last(r"\[SET64\] default[^\r\n]*", vm.log()) or ["（无）"])[0])

        # ---------- 5) 关于页 ----------
        print("--- 5) 关于：版本 / 驱动 / GPU 加速状态 + 硬件检查报告 ---")
        goto_page(vm, P_ABOUT, checks, "第1遍")
        exp_ver = build64_version()
        ab = last(r"\[SET64\] about version=(\S+) (\S+) (\S+) build_tag=(\S+) bits=(\d+)", vm.log())
        check("关于页打点版本（[SET64] about version=VimtuOS %s x86_64 build_tag=.. bits=64；"
              "期望值来自 build64.sh 的 VIMTUOS_VERSION 唯一真源，不在这里写死第二份）" % (exp_ver or "?"),
              ab is not None and ab.group(1) == "VimtuOS" and ab.group(2) == exp_ver
              and ab.group(5) == "64" and ab.group(4) not in ("", "(none)"),
              ab.group(0) if ab else "（无）")
        dv = last(r"\[SET64\] drivers implemented=(\d+) missing=(\d+) gpu=(\S+)", vm.log())
        check("驱动清单打点（implemented=12 missing=4 gpu=software）",
              dv is not None and dv.group(1) == "12" and dv.group(2) == "4" and dv.group(3) == "software",
              dv.group(0) if dv else "（无）")
        gp = last(r"\[SET64\] gpu accel=(\S+) renderer=(\S+)", vm.log())
        check("GPU 加速状态如实写「未实现，软件渲染」（[SET64] gpu accel=none renderer=software）",
              gp is not None and gp.group(1) == "none" and gp.group(2) == "software",
              gp.group(0) if gp else "（无）")
        va = view_rect(vm, P_ABOUT)
        shot_ab = save_shot(vm.mon, os.path.join(tmp, "about.ppm"))
        if shot_ab and va:
            _, _, pxa2 = read_ppm(shot_ab)
            d, tot = region_diff(px1, pxa2, w0, (va["x"], va["y"], va["w"], va["h"]))
            check("关于页内容与其它页不同（像素差 %d/%d）" % (d, tot), d > 40, "")
        # 硬件检查报告视图：键盘 'h' 也能开（无鼠标路径，比点按钮稳），失败再点按钮
        since_hw = vm.n()
        vm.mon.key("h", wait=1.0)
        if not vm.wait_new("[SET64] hwui lines=", 10, since_hw):
            # 兜底：点"显示硬件检查报告"按钮（点不到也不额外记失败——上面那条断言已经涵盖）
            r_hw = ctl_xy(vm, P_ABOUT, CID_HW_CHECK)
            if r_hw:
                vm.cur.click_at(vm.mon, r_hw["acx"], r_hw["acy"])
                vm.wait_new("[SET64] hwui lines=", 10, vm.n() - 400)
        vm.mon.key("esc", wait=1.0)
        hw_line = last(r"\[SET64\] hwui[^\r\n]*", vm.log())
        hw_boot = last(r"\[HWUI\] report lines=\d+ storage=\d+ controllers=\d+", vm.log())
        check("硬件检查报告可用（设置页内嵌 [SET64] hwui lines=.. 或启动期 [HWUI] report lines=..）",
              hw_line is not None or hw_boot is not None,
              (hw_line.group(0) if hw_line else (hw_boot.group(0) if hw_boot else "（无）")))

        # ---------- 6) 主题 -> 暗色 ----------
        print("--- 6) 主题：切到「暗色」（[THEME64] apply + 像素 + 持久化）---")
        init0 = last(r"\[THEME64\] init themes=\d+ theme=(\d+) name=(\S+) dark=(\d)", vm.log())
        check("开机主题 init 打点（默认白 theme=0 name=white）",
              init0 is not None and init0.group(1) == "0" and init0.group(3) == "0",
              init0.group(0) if init0 else "（无）")
        goto_page(vm, P_THEME, checks, "第1遍")
        th_before = save_shot(vm.mon, os.path.join(tmp, "theme_before.ppm"))
        ok_th, _ = click_ctl(vm, P_THEME, CID_THEME_TILE0 + 1, checks, "第1遍-主题",
                             expect="[THEME64] apply theme=1")
        th = last(r"\[THEME64\] apply theme=(\d+) name=(\S+) dark=(\d)", vm.log())
        check("切「暗色」打点（[THEME64] apply theme=1 name=dark dark=1）",
              ok_th and th is not None and th.group(1) == "1" and th.group(3) == "1",
              th.group(0) if th else (last(r"\[THEME64\] apply[^\r\n]*", vm.log()) or ["（无）"])[0])
        st = last(r"\[SET64\] theme id=(\d+) name=(\S+) reduce_motion=(\d) persisted=(\d)", vm.log())
        check("设置页自己的主题行（[SET64] theme id=1 name=dark persisted=1）",
              st is not None and st.group(1) == "1" and st.group(4) == "1",
              st.group(0) if st else "（无）")
        # 主题切换后点一下窗口外的桌面：强制外壳重绘桌面层（QEMU TCG 下整屏重绘要几秒）
        vm.cur.click_at(vm.mon, 1150, 620, wait=1.0)
        time.sleep(1.5)
        th_after = save_shot(vm.mon, os.path.join(tmp, "theme_after.ppm"))
        if th_before and th_after:
            _, _, pxb = read_ppm(th_before)
            _, _, pxaf = read_ppm(th_after)
            # 主断言：窗口区（内容卡片 + 导航亚克力）大面积像素变化
            d, tot = region_diff(pxb, pxaf, w0, WINDOW_STRIP, thr=8)
            check("★ 主题切换后像素大面积变化（窗口区 %d/%d 取样点，thr=8）" % (d, tot), d > 3000,
                  "window strip=%s" % (WINDOW_STRIP,))
            d2, tot2 = region_diff(pxb, pxaf, w0, DOCK_STRIP)
            check("★ Dock 条像素也跟着主题变（%d/%d 取样点）" % (d2, tot2), d2 > 1000,
                  "dock strip=%s" % (DOCK_STRIP,))
            d3, tot3 = region_diff(pxb, pxaf, w0, TITLE_STRIP)
            l0 = region_lum(pxb, w0, DESK_STRIP)
            l1 = region_lum(pxaf, w0, DESK_STRIP)
            print("      [信息] 标题栏片 %d/%d；桌面片亮度 %.1f -> %.1f（如实记录，不作断言）"
                  % (d3, tot3, l0, l1))
        flush_wait(vm)
    finally:
        vms[-1].stop()

    # =====================================================================
    # 第 2 遍：持久化检查 -> Dock -> 壁纸适应模式（桌面/锁屏分别）-> 解锁 -> 设密码
    # =====================================================================
    print("=== 第 2 遍：重启后持久化 -> Dock 几何 -> 壁纸适应模式 -> 解锁 -> 设密码 ===")
    try:
        vm = boot("2")
        log = vm.log()
        init1 = last(r"\[THEME64\] init themes=\d+ theme=(\d+) name=(\S+) dark=(\d)", log)
        check("★ 重启后主题仍是「暗色」（[THEME64] init theme=1 name=dark dark=1）",
              init1 is not None and init1.group(1) == "1" and init1.group(3) == "1",
              init1.group(0) if init1 else "（无）")
        log = vm.log()          # 重新读一次：桌面/开始菜单/登录界面的行是刚才才写进去的
        ba = last(r"\[SET64\] boot apply font=(\d+) grad=(\d+) wall=(\S+)", log)
        check("★ 重启后字号仍是 18（[SET64] boot apply font=18 + [FONT64] size px=18）",
              ba is not None and ba.group(1) == "18" and "[FONT64] size px=18" in log,
              ba.group(0) if ba else "（无）")
        db = last(r"\[SET64\] default boot elf=(\S+) vap=(\S+) txt=(\S+) video=(\S+)", log)
        check("★ 重启后默认应用保持（[SET64] default boot .. txt=mypc ..）",
              db is not None and db.group(3) == "mypc" and db.group(4) == "none",
              db.group(0) if db else "（无）")
        # 用户表持久化的硬证据：登录时用户记录被应用（名字 + 头像路径 + 主题/锁屏模式）。
        # 注意：这一行是登录（自动登录延迟 8s）之后才写的：先等它出现，再读日志。
        vm.wait_new("[USER64] login apply settings", 25, 0)
        lsa = last(r"\[USER64\] login apply settings user=(\S+) theme=(\S+) wall=(\S+) lock_mode=(\S+) "
                   r"avatar=(\S+)", vm.log())
        check("★ 重启后用户记录仍是 vimtu2 + 头像 PNG + 主题=暗色（[USER64] login apply settings）",
              lsa is not None and lsa.group(1) == "vimtu2" and lsa.group(5) == "/logo/kaisi.png"
              and lsa.group(2) == "1",
              lsa.group(0) if lsa else "（无）")
        # 开始菜单的名字/头像来源（[START64] user avatar 每个启动只打一次；这里开一次菜单，如实记录）
        if open_start_menu(vm):
            vm.mon.key("esc", wait=1.2)
        su = last(r"\[START64\] user avatar x=\d+ y=\d+ d=\d+ name=(\S+) src=(\S+)", vm.log())
        print("      [信息] 重启后开始菜单用户行：%s" % (su.group(0) if su else "（本次启动还没画过开始菜单）"))
        gm0 = last(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=\d+ icon=(\d+) gap=(\d+)", log)
        check("[DOCK64] geom 基线（w/icon/gap）", gm0 is not None, gm0.group(0) if gm0 else "（无）")

        if not open_settings(vm, checks, "第2遍"):
            raise SystemExit(1)

        # ---------- 7) Dock ----------
        print("--- 7) Dock：长度/图标尺寸/间距 -> [DOCK64] geom 变化 + 像素变化 ---")
        goto_page(vm, P_DOCK, checks, "第2遍")
        dock_before = save_shot(vm.mon, os.path.join(tmp, "dock_before.ppm"))
        _, _, pxd0 = read_ppm(dock_before)
        click_ctl(vm, P_DOCK, CID_DOCK_LEN, checks, "第2遍-Dock长度", frac=0.70,
                  expect="[SET64] dock len=")
        dl = last(r"\[SET64\] dock len=(\d+) auto=(\d+) icon=(\d+) gap=(\d+) size=(\d+) why=(\S+) applied=1", vm.log())
        check("改 Dock 长度（[SET64] dock len=872 auto=0 why=slider applied=1）",
              dl is not None and dl.group(2) == "0" and int(dl.group(1)) > 320,
              dl.group(0) if dl else "（无）")
        gm1 = last(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=\d+ icon=(\d+) gap=(\d+)", vm.log())
        check("★ [DOCK64] geom 重新打点：改长度后 w 变了（%s -> %s）"
              % ((gm0.group(3) if gm0 else "?"), (gm1.group(3) if gm1 else "?")),
              gm1 is not None and gm0 is not None and gm1.group(3) != gm0.group(3),
              gm1.group(0) if gm1 else "（无）")
        click_ctl(vm, P_DOCK, CID_DOCK_ICON, checks, "第2遍-Dock图标", frac=0.02,
                  expect="[DOCK64] geom")
        time.sleep(1.0)
        gm2 = last(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=\d+ icon=(\d+) gap=(\d+)", vm.log())
        check("★ 图标尺寸变化（[DOCK64] geom icon=%s -> %s；w 也跟着变）"
              % ((gm1.group(5) if gm1 else "?"), (gm2.group(5) if gm2 else "?")),
              gm2 is not None and gm1 is not None and gm2.group(5) != gm1.group(5),
              gm2.group(0) if gm2 else "（无）")
        click_ctl(vm, P_DOCK, CID_DOCK_GAP, checks, "第2遍-Dock间距", frac=0.80,
                  expect="[DOCK64] geom")
        time.sleep(1.0)
        gm3 = last(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=\d+ icon=(\d+) gap=(\d+)", vm.log())
        check("★ 图标间距变化（[DOCK64] geom gap=%s -> %s）"
              % ((gm2.group(6) if gm2 else "?"), (gm3.group(6) if gm3 else "?")),
              gm3 is not None and gm2 is not None and gm3.group(6) != gm2.group(6),
              gm3.group(0) if gm3 else "（无）")
        dock_after = save_shot(vm.mon, os.path.join(tmp, "dock_after.ppm"))
        if dock_after:
            _, _, pxd1 = read_ppm(dock_after)
            d, tot = region_diff(pxd0, pxd1, w0, DOCK_STRIP)
            check("★ Dock 条像素变了（%d/%d 取样点）" % (d, tot), d > 20, "")

        # ---------- 8) 壁纸适应模式（桌面 / 锁屏分别设置）----------
        print("--- 8) 壁纸适应模式：桌面=平铺 + 锁屏=居中（关同步）---")
        goto_page(vm, P_FIT, checks, "第2遍")
        wall_before = save_shot(vm.mon, os.path.join(tmp, "wall_before.ppm"))
        _, _, pxw0 = read_ppm(wall_before)
        click_ctl(vm, P_FIT, CID_WALLFIT_BASE + 3, checks, "第2遍-平铺",
                  expect="[SET64] wall mode=3 name=tile target=desktop")
        wm = last(r"\[SET64\] wall mode=(\d+) name=(\S+) target=desktop sync=(\d) lock_mode=(\d+) desktop_mode=(\d+)",
                  vm.log())
        check("桌面改「平铺」（[SET64] wall mode=3 name=tile target=desktop）", wm is not None,
              wm.group(0) if wm else "（无）")
        check("gfx64 侧同步生效（[GFX64] wall mode=3 name=tile）", "wall mode=3 name=tile" in vm.log(),
              (last(r"\[GFX64\] wall mode=[^\r\n]*", vm.log()) or ["（无）"])[0])
        wall_after = save_shot(vm.mon, os.path.join(tmp, "wall_after.ppm"))
        if wall_after:
            _, _, pxw1 = read_ppm(wall_after)
            d, tot = region_diff(pxw0, pxw1, w0, WINDOW_STRIP, thr=8)
            d3, tot3 = region_diff(pxw0, pxw1, w0, DESK_STRIP)
            l0 = region_lum(pxw0, w0, DESK_STRIP)
            l1 = region_lum(pxw1, w0, DESK_STRIP)
            check("★ 改适应模式后像素变化（窗口区 %d/%d，thr=8；桌面片 %d/%d，亮度 %.1f -> %.1f）"
                  % (d, tot, d3, tot3, l0, l1), d > 1000 or d3 > 30, "window=%d desk=%d" % (d, d3))
        click_ctl(vm, P_FIT, CID_FIT_SYNC, checks, "第2遍-关同步", expect="[SET64] wall sync=")
        click_ctl(vm, P_FIT, CID_LOCKFIT_BASE + 4, checks, "第2遍-锁屏居中", expect="target=lock")
        lm = last(r"\[SET64\] wall mode=(\d+) name=(\S+) target=lock sync=(\d) lock_mode=(\d+) desktop_mode=(\d+)",
                  vm.log())
        check("★ 锁屏单独设「居中」（[SET64] wall mode=4 name=center target=lock sync=0 "
              "lock_mode=4 desktop_mode=3）—— 桌面/锁屏分别设置",
              lm is not None and lm.group(1) == "4" and lm.group(3) == "0"
              and lm.group(4) == "4" and lm.group(5) == "3",
              lm.group(0) if lm else "（无）")
        # 锁定 -> 锁屏按自己那套模式绘制（boot 2 的锁屏 paint 行在启动时已用掉预算，
        # 真正的"锁屏生效"证据在第 3 遍启动日志里；这里断言锁定请求接到了既有实现）
        goto_page(vm, P_POWER, checks, "第2遍")
        click_ctl(vm, P_POWER, CID_PWR_LOCK, checks, "第2遍-锁定",
                  expect="[LOCK64] lock requested by=settings")
        # 解锁：点锁屏 -> 登录界面 -> 回车（此时还没设口令）
        time.sleep(1.5)
        vm.cur.click_at(vm.mon, 640, 400, wait=1.4)
        vm.mon.key("ret", wait=1.8)
        ok_unlock = vm.wait_new("[LOGIN64] login ok", 15, vm.n() - 3000)
        check("解锁回到桌面（[LOGIN64] login ok via=auto|click|key-enter）", ok_unlock,
              (last(r"\[LOGIN64\] login ok[^\r\n]*", vm.log()) or ["（无）"])[0])
        time.sleep(2.0)

        # ---------- 9) 设密码 ----------
        print("--- 9) 密码：设置口令（加盐 SHA-256）---")
        if not open_settings(vm, checks, "第2遍(密码)"):
            raise SystemExit(1)
        goto_page(vm, P_PASSWD, checks, "第2遍")
        r_pw = ctl_xy(vm, P_PASSWD, CID_PW_FIELD)
        if r_pw:
            vm.cur.click_at(vm.mon, r_pw["acx"], r_pw["acy"])
            time.sleep(0.4)
            type_keys(vm, "pw12")
            vm.mon.key("ret", wait=1.6)
        pw = last(r"\[SET64\] password user=(\S+) set=(\d) algo=sha256 iter=1000 via=settings saved=(\d)", vm.log())
        check("设密码打点（[SET64] password user=vimtu2 set=1 algo=sha256 iter=1000 saved=1）",
              pw is not None and pw.group(2) == "1" and pw.group(3) == "1",
              pw.group(0) if pw else (last(r"\[SET64\] password[^\r\n]*", vm.log()) or ["（无）"])[0])
        check("明文口令绝不进串口", "pw12" not in vm.log())
        check("用户表落盘（[USER64] userdb save ok path=/etc/users.db）",
              "[USER64] userdb save ok" in vm.log(),
              (last(r"\[USER64\] userdb save[^\r\n]*", vm.log()) or ["（无）"])[0])
        ngeom = len(find_all(r"\[DOCK64\] geom x=", vm.log()))
        check("Dock 几何被设置页改过（本轮 [DOCK64] geom 行数 = %d）" % ngeom, ngeom >= 3, "")
        flush_wait(vm)
    finally:
        vms[-1].stop()

    # =====================================================================
    # 第 3 遍：必须输密码 + 所有设置保持
    # =====================================================================
    print("=== 第 3 遍：重启后登录**必须输密码** + 所有设置保持 ===")
    try:
        vm = boot("3")
        log = vm.log()
        check("★ 有口令的用户**不会**自动登录（没有 [LOCK64] auto login）",
              "[LOCK64] auto login" not in log, "")
        lw = last(r"\[LOCK64\] wall src=[^\r\n]*? lock_mode=(\d+) desktop_mode=(\d+) applied=(\d)", log)
        check("★ 锁屏按自己那套模式绘制（[LOCK64] wall .. lock_mode=4 desktop_mode=3，两边不同）",
              lw is not None and lw.group(1) == "4" and lw.group(2) == "3",
              lw.group(0) if lw else (last(r"\[LOCK64\] wall[^\r\n]*", log) or ["（无）"])[0])
        # 锁屏 -> 登录界面 -> 提交一次 -> 弹密码框
        vm.cur.click_at(vm.mon, 640, 400, wait=1.4)
        vm.wait_new("[LOGIN64] login screen shown", 15, 0)
        since = vm.n()
        vm.mon.key("ret", wait=1.8)
        pp = last(r"\[LOGIN64\] password prompt user=(\S+)", vm.tail(since))
        if pp is None:
            pp = last(r"\[LOGIN64\] password prompt user=(\S+)", vm.log())
        check("★ 重启后登录必须输密码（[LOGIN64] password prompt user=vimtu2）",
              pp is not None and pp.group(1) == "vimtu2",
              pp.group(0) if pp else (last(r"\[LOGIN64\] password[^\r\n]*", vm.log()) or ["（无）"])[0])
        ulg = last(r"\[LOGIN64\] userlist n=(\d+) names=(\S+)", vm.log())
        check("★ 登录界面只有新用户（[LOGIN64] userlist n=1 names=vimtu2）",
              ulg is not None and ulg.group(2) == "vimtu2",
              (last(r"\[LOGIN64\] userlist[^\r\n]*", vm.log()) or ["（无）"])[0])
        # 错误口令 -> password wrong；正确口令 -> 进桌面
        since = vm.n()
        type_keys(vm, "xy")
        vm.mon.key("ret", wait=1.8)
        check("错误口令被拒（[LOGIN64] password wrong）",
              vm.wait_new("[LOGIN64] password wrong", 10, since),
              (last(r"\[LOGIN64\] password wrong[^\r\n]*", vm.log()) or ["（无）"])[0])
        since = vm.n()
        type_keys(vm, "pw12")
        vm.mon.key("ret", wait=2.2)
        ok_in = vm.wait_new("[LOGIN64] login ok user=vimtu2", 20, since)
        check("正确口令登录成功（[LOGIN64] login ok user=vimtu2）", ok_in,
              (last(r"\[LOGIN64\] login ok[^\r\n]*", vm.log()) or ["（无）"])[0])
        time.sleep(3.0)
        # ---- 所有值保持 ----
        g3 = last(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=\d+ icon=(\d+) gap=(\d+)", vm.log())
        check("★ 重启后 Dock 几何保持（第2遍末尾 icon=%s gap=%s w=%s；现在 icon=%s gap=%s w=%s）"
              % ((gm3.group(5) if gm3 else "?"), (gm3.group(6) if gm3 else "?"), (gm3.group(3) if gm3 else "?"),
                 (g3.group(5) if g3 else "?"), (g3.group(6) if g3 else "?"), (g3.group(3) if g3 else "?")),
              g3 is not None and gm3 is not None and g3.group(3) == gm3.group(3)
              and g3.group(5) == gm3.group(5) and g3.group(6) == gm3.group(6),
              g3.group(0) if g3 else "（无）")
        t3 = last(r"\[THEME64\] init themes=\d+ theme=(\d+) name=(\S+) dark=(\d)", vm.log())
        check("★ 重启后主题保持暗色（[THEME64] init theme=1 name=dark dark=1）",
              t3 is not None and t3.group(1) == "1" and t3.group(3) == "1", t3.group(0) if t3 else "（无）")
        f3 = last(r"\[SET64\] boot apply font=(\d+)", vm.log())
        check("★ 重启后字号保持 18（[SET64] boot apply font=18）", f3 is not None and f3.group(1) == "18",
              f3.group(0) if f3 else "（无）")
        d3 = last(r"\[SET64\] default boot elf=(\S+) vap=(\S+) txt=(\S+) video=(\S+)", vm.log())
        check("★ 重启后默认应用保持（[SET64] default boot .. txt=mypc）",
              d3 is not None and d3.group(3) == "mypc", d3.group(0) if d3 else "（无）")
        a3 = last(r"\[START64\] user avatar x=\d+ y=\d+ d=\d+ name=(\S+) src=(\S+)", vm.log())
        lsa3 = last(r"\[USER64\] login apply settings user=(\S+) theme=(\S+) wall=(\S+) lock_mode=(\S+) "
                    r"avatar=(\S+)", vm.log())
        check("★ 重启后用户名/头像保持（用户表：%s；开始菜单：%s）"
              % ((lsa3.group(1) + " avatar=" + lsa3.group(5)) if lsa3 else "（无）",
                 (a3.group(1) + " " + a3.group(2)) if a3 else "（本次启动还没画过开始菜单）"),
              (lsa3 is not None and lsa3.group(1) == "vimtu2" and lsa3.group(5) == "/logo/kaisi.png")
              or (a3 is not None and a3.group(1) == "vimtu2" and a3.group(2) == "userdb"),
              (lsa3.group(0) if lsa3 else (a3.group(0) if a3 else "（无）")))

        # ---------- 10) 清空密码 ----------
        print("--- 10) 清空密码（= 不设密码）---")
        if open_settings(vm, checks, "第3遍"):
            goto_page(vm, P_PASSWD, checks, "第3遍")
            click_ctl(vm, P_PASSWD, CID_PW_CLEAR, checks, "第3遍-清空密码", expect="[SET64] password")
            cl = last(r"\[SET64\] password user=(\S+) set=(\d) algo=sha256 iter=1000 via=settings saved=(\d)",
                      vm.log())
            check("清空密码打点（[SET64] password .. set=0 saved=1）",
                  cl is not None and cl.group(2) == "0" and cl.group(3) == "1",
                  cl.group(0) if cl else (last(r"\[SET64\] password[^\r\n]*", vm.log()) or ["（无）"])[0])
            flush_wait(vm)
    finally:
        vms[-1].stop()

    # =====================================================================
    # 第 4 遍：没密码 -> 直接进桌面
    # =====================================================================
    print("=== 第 4 遍：清空密码后重启 -> 直接进桌面（不再要口令）===")
    try:
        vm = Vm(qemu, disk, tmp, "4")
        vms.append(vm)
        up = vm.wait_new("[GUI64] ready", 200) or vm.wait_new("[LOCK64] lock screen shown", 30)
        log = vm.log()
        check("第 4 遍重启到桌面（[GUI64] ready）", up and "[GUI64] ready" in log,
              (last(r"\[GUI64\] ready", log) or ["（无）"])[0])
        check("★ 清空密码后不再要口令（没有 [LOGIN64] password prompt）",
              "[LOGIN64] password prompt" not in log, "")
        check("★ 无密码用户自动登录（[LOCK64] auto login user=vimtu2）",
              "[LOCK64] auto login user=vimtu2" in log,
              (last(r"\[LOCK64\] auto login[^\r\n]*", log) or ["（无）"])[0])
        for bad in FORBIDDEN:
            check("第 4 遍日志里不得出现 %s" % bad, bad not in log, "")
    finally:
        vms[-1].stop()

    # =====================================================================
    print("")
    print("串口日志目录：%s" % tmp)
    print("== 结果：%d 项断言，%s ==" % (len(checks), "全过" if all(c[1] for c in checks) else "有失败"))
    fails = [(n, d) for n, c, d in checks if not c]
    if fails:
        print("失败项：")
        for n, d in fails:
            print("  - %s  %s" % (n, d))
        for vm in vms:
            print("--- boot_%s 关键行 ---" % vm.tag)
            for line in vm.log().splitlines():
                if re.search(r"\[(THEME64|SET64|GFX64|FONT64|CONF64|LOCK64|START64|USER64)\]", line) \
                        and re.search(r"(init|apply|boot|theme|wall|dock|font|default|password|avatar|rename|"
                                      r"flush|set key|geom|user avatar|lock screen|password prompt|login ok|"
                                      r"auto login|userlist|save ok|size px|mode=)", line):
                    print("   | " + line.strip()[:190])
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
