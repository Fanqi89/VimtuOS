#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/icons64_test.py - ★ 本批验收：外置图标包（真图标）加载 / 像素 / 回落 / 主题跟随 / Dock 开始按钮

覆盖（每条都要 串口打点 + 像素 证据；判据强度不削弱）：
  ① 首次加载：`[ICON64] init pack lba=.. bytes=.. entries=.. icons=.. bad=0 ok=1` +
     **每一个** kind 都有一条 `[ICON64] load kind=<名字> path=pack:…@<尺寸>.png src=pack ok=1`，
     且"加载到的 kind 集合"与宿主侧 build/icons/manifest.json 的图标清单**完全相等**（不重不漏）；
     每条 load 的 path/尺寸 = 清单里"不小于请求的最小档"（只缩小，最清晰）；
     像素：开始菜单状态区 3 个图标（以太网/声音/通知）区域的墨迹形状与宿主侧预渲染 PNG
     （按内核同一个最近邻公式采样）形状一致 —— IoU 阈值 0.55。
  ② 回落（两种坏法都试）：
     (a) 把盘上 `apps/terminal@*` 三个条目 + `apps/mypc@48` 一个条目的字节改坏 ->
         `[ICON64] init pack … bad=4 ok=0`；terminal 没有可用条目 -> `[ICON64] fallback kind=terminal
         reason=no-entry (programmatic draw kept)`；mypc 只坏了大档 -> 自动改用 32px 档
         （`[ICON64] load kind=mypc path=pack:apps/mypc@32.png … ok=1`，优雅降级）；
         界面不空：终端 Dock 图标区域仍有墨迹（内嵌程序化图标）。
     (b) 把包 magic 清零 -> `[ICON64] init pack absent …` + 每个 kind 都打 fallback 点 +
         状态区图标区域仍有墨迹（程序化线性图标）、Dock 我的电脑图标仍有墨迹、开始按钮照常。
  ③ Dock 开始按钮仍是真图：`[DOCK64] start icon src=vfs:/logo/kaisi.png size=46 ok=1`
     （需要带 VimtuFS2 卷的系统盘；本脚本自己造夹具盘）+ 该区域与 logo/kaisi.png 的形状 IoU>=0.55。
  ④ 状态区图标颜色跟随主题 Token（不是硬编码）：白色主题下墨迹偏深、切到暗色主题（Ctrl+Shift+T）后
     同一区域墨迹变浅（亮度上升 >= 60），且形状 IoU 仍达标（同一个图标只换颜色）。

用法：py -3 tests\\icons64_test.py [--qemu 路径] [--port 5677] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import json
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import startmenu64_test as smt   # noqa: E402  （复用它的 Monitor/Cursor 鼠标模型：其他验收同款）

QEMU_CANDIDATES = smt.QEMU_CANDIDATES
PORT = 5677
PACK_LBA = 7497          # 与 kernel/icons64.h 的 ICON64_PACK_LBA（= 9 + 8000 - 512）一致
SECTOR = 512
SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
MANIFEST = os.path.join(ROOT, "build", "icons", "manifest.json")
ICON_DIR = os.path.join(ROOT, "build", "icons")
KAISI = os.path.join(ROOT, "logo", "kaisi.png")
SM_ICON = 22             # THEME64_SM_ICON：状态区图标边长

# 应用图标的"品牌感"亮色（与 kernel/icons64.cpp 的 icons64_app_color64 一致）
APP_COLOR = {
    "mypc": (0x7F, 0xD1, 0xFF), "recycle": (0x86, 0xE2, 0xC0), "terminal": (0x9C, 0xE3, 0x8A),
    "settings": (0xC3, 0xD0, 0xFF), "tmgr": (0xFF, 0xC9, 0x78), "mines": (0xFF, 0x9E, 0x9E),
    "calc": (0x8F, 0xE9, 0xE0), "files": (0xFF, 0xD9, 0x8F), "monitor-app": (0x9F, 0xD8, 0xFF),
    "about": (0xD6, 0xC4, 0xFF),
}


# ==================== 像素工具（与内核同口径）====================
def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def region(px, w, x0, y0, n):
    return [[sample(px, w, x0 + i, y0 + j) for i in range(n)] for j in range(n)]


def median_color(vals):
    rs = sorted(v[0] for v in vals)
    gs = sorted(v[1] for v in vals)
    bs = sorted(v[2] for v in vals)
    m = len(vals) // 2
    return rs[m], gs[m], bs[m]


def ink_mask(px, w, x0, y0, n, thresh, ring=3):
    """图标区域的墨迹掩码。底色从**图标框外面一圈**估（图标本身很密时，边框像素会全落在图案里，
    用框内边框估底色会估成图案色 —— 以太网这种"实心圆角块"图标就会误判）。"""
    vals = []
    for i in range(-ring, n + ring):
        vals.append(sample(px, w, x0 + i, y0 - ring))
        vals.append(sample(px, w, x0 + i, y0 + n - 1 + ring))
    for j in range(-ring, n + ring):
        vals.append(sample(px, w, x0 - ring, y0 + j))
        vals.append(sample(px, w, x0 + n - 1 + ring, y0 + j))
    bg = median_color(vals)
    g = region(px, w, x0, y0, n)
    return [[1 if dist(g[j][i], bg) > thresh else 0 for i in range(n)] for j in range(n)]


def ink_count(m):
    return sum(sum(r) for r in m)


def iou(a, b):
    inter = uni = 0
    for j in range(len(a)):
        for i in range(len(a[j])):
            if a[j][i] and b[j][i]:
                inter += 1
            if a[j][i] or b[j][i]:
                uni += 1
    return (inter / float(uni)) if uni else 1.0


def host_mask(png_path, n):
    """宿主侧 PNG -> n×n 的 0/1 掩码（**内核同一个最近邻公式**：sx = i*w/n、sy = j*h/n）"""
    from PIL import Image
    im = Image.open(png_path).convert("RGBA")
    im.load()
    w, h = im.size
    m = []
    for j in range(n):
        sy = int(j * h / n)
        row = []
        for i in range(n):
            sx = int(i * w / n)
            row.append(1 if im.getpixel((sx, sy))[3] > 128 else 0)
        m.append(row)
    return m


def color_ratio(px, w, x0, y0, n, rgb, tol=60):
    g = region(px, w, x0, y0, n)
    return sum(1 for j in range(n) for i in range(n) if dist(g[j][i], rgb) <= tol) / float(n * n)


def ring_bg(px, w, x0, y0, n, ring=3):
    """图标框**外面一圈**的中位色（= 面板/底板色）"""
    vals = [sample(px, w, x0 + i, y0 - ring) for i in range(n)]
    vals += [sample(px, w, x0 + i, y0 + n - 1 + ring) for i in range(n)]
    return median_color(vals)


def ink_color(px, w, x0, y0, n, thresh=60):
    """区域里"非底色"像素的中位色 + 数量（用来证明颜色随主题 Token 变）"""
    bg = ring_bg(px, w, x0, y0, n)
    g = region(px, w, x0, y0, n)
    vals = [g[j][i] for j in range(n) for i in range(n) if dist(g[j][i], bg) > thresh]
    if not vals:
        return None, 0
    return median_color(vals), len(vals)


# ==================== 夹具盘 ====================
def make_fixture(tmp, name, corrupt=None):
    """夹具盘：16MB 系统盘（system.img 字节 + MBR + @8009 的 v3 卷）。
    卷存在时开始按钮才会从 VimtuFS2 读 /logo/kaisi.png（"src=vfs:/logo/kaisi.png"这条验收靠它）。
    图标包在 system.img 的内核区尾部（LBA 7497），因此**原样保留**在夹具盘上。
    corrupt: None | 'crc'（改坏 terminal 三个档 + mypc@48）| 'magic'（清零包 magic）"""
    import fs_tree_test as fst
    path = os.path.join(tmp, name)
    if fst.make_small_system_disk(path) is None:
        raise RuntimeError("造夹具盘失败（build64/system.img 缺失或过大）")
    if not corrupt:
        return path
    with open(path, "r+b") as f:
        if corrupt == "magic":
            f.seek(PACK_LBA * SECTOR)
            f.write(b"\x00" * 8)                     # magic 清零 -> 整包不可用
            return path
        man = json.load(open(MANIFEST, encoding="utf-8"))
        hit = [e for e in man["entries"]
               if e["name"] == "terminal" or (e["name"] == "mypc" and e["size"] == 48)]
        if len(hit) < 4:
            raise RuntimeError("清单里找不到要改坏的条目（terminal×3 + mypc@48）")
        for e in hit:
            pos = PACK_LBA * SECTOR + e["off"] + e["len"] // 2
            f.seek(pos)
            b = f.read(1)
            f.seek(pos)
            f.write(bytes([b[0] ^ 0xFF]))            # 翻一个字节 -> CRC32 必失配
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2
    if not os.path.exists(MANIFEST):
        sys.stderr.write("图标清单不存在：%s（先跑 bash build64.sh）\n" % MANIFEST)
        return 2
    qemu = smt.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    man = json.load(open(MANIFEST, encoding="utf-8"))
    by_kind = {}
    for e in man["entries"]:
        by_kind.setdefault(e["name"], []).append(e)
    for v in by_kind.values():
        v.sort(key=lambda x: x["size"])
    pack_bytes = man["pack_bytes"]

    tmp = tempfile.mkdtemp(prefix="vimtu64_icons64_")
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def boom():
        print("\n===== 汇总：%d 项断言，失败 %d 项 =====" % (len(checks), 1))
        return 2

    # =============================================================
    print("=== 0) 夹具盘（16MB 系统盘 + VimtuFS2 卷）启动 ===")
    img1 = make_fixture(tmp, "icons_ok.img")
    vm = smt.Vm(qemu, img1, args.port, "vimtu-icons64", tmp)
    try:
        mon = vm.wait_monitor()
        # ★ 缺陷 4（登录必须显式输入）：默认不再自动登录 —— 先等锁屏可交互，再回车两次进桌面。
        vm.wait_log("[LOCK64] bg blur ready", 150)
        mon.key("ret", wait=1.0)          # 锁屏 -> 登录界面
        mon.key("ret", wait=1.5)          # 登录按钮（无密码用户）
        up = vm.wait_log("[GUI64] ready", 180)
        check("桌面就绪（[GUI64] ready）", up)
        if not up:
            return boom()
        log = vm.log()

        print("=== 1) ① 图标包加载（打点 + 与宿主清单逐项核对）===")
        init = re.search(r"\[ICON64\] init pack lba=(\d+) drive=(\d+) bytes=(\d+) entries=(\d+) "
                         r"icons=(\d+) bad=(\d+) ok=(\d) fnv=([0-9a-f]{8}) vfs_icons=(\d)", log)
        check("[ICON64] init pack 打点（lba/drive/bytes/entries/icons/bad/ok）", init is not None,
              init.group(0) if init else "（无）")
        if init:
            check("包位置 = 内核区尾部 LBA %d（与 icons64.h 一致）" % PACK_LBA, int(init.group(1)) == PACK_LBA,
                  "lba=%s" % init.group(1))
            check("包大小 = 宿主 build/iconpack.bin（%d B）" % pack_bytes, int(init.group(3)) == pack_bytes,
                  "内核读到 bytes=%s" % init.group(3))
            check("没有坏条目（bad=0 ok=1）", init.group(6) == "0" and init.group(7) == "1",
                  "bad=%s ok=%s" % (init.group(6), init.group(7)))
            check("kind 数 = 宿主清单图标数（%d）" % len(by_kind), int(init.group(5)) == len(by_kind),
                  "icons=%s" % init.group(5))

        loaded = {}
        for m in re.finditer(r"\[ICON64\] load kind=(\S+) path=(\S+) size=(\d+) src=(\S+) ok=(\d)", log):
            loaded.setdefault(m.group(1), []).append(m)
        check("每个图标都有 [ICON64] load 打点（宿主清单 %d 个 kind）" % len(by_kind),
              sorted(loaded) == sorted(by_kind),
              "内核 %d 个；缺 %s 多 %s" % (len(loaded), sorted(set(by_kind) - set(loaded)),
                                          sorted(set(loaded) - set(by_kind))))
        check("所有 load 都是 ok=1 且 src=pack",
              all(m.group(5) == "1" and m.group(4) == "pack" for v in loaded.values() for m in v),
              str(sorted({(m.group(4), m.group(5)) for v in loaded.values() for m in v})))
        bad_path = []
        for name, v in loaded.items():
            m = v[0]
            sizes = [e["size"] for e in by_kind[name]]
            want = min([s for s in sizes if s >= (48 if name in APP_COLOR else 24)] or [max(sizes)])
            sub = "apps" if name in APP_COLOR else "system"
            if m.group(2) != "pack:%s/%s@%d.png" % (sub, name, want) or int(m.group(3)) != want:
                bad_path.append("%s: %s@%s (want %d)" % (name, m.group(2), m.group(3), want))
        check("load 的 path/尺寸 = 清单里「不小于请求的最小档」（只缩小最清晰）", not bad_path,
              "; ".join(bad_path[:4]))
        st = re.search(r"\[ICON64\] selftest (PASS|FAIL) mask=(\d+) pack=(\d+)", log)
        check("[ICON64] selftest PASS mask=0", st is not None and st.group(1) == "PASS" and st.group(2) == "0",
              st.group(0) if st else "（无）")
        check("预加载证据：每个 kind 至少一条 load", len(loaded) >= len(by_kind),
              "kind=%d 行=%d" % (len(loaded), sum(len(v) for v in loaded.values())))

        print("=== 2) ③ Dock 开始按钮仍是真图 logo/kaisi.png（VimtuFS2 优先）===")
        si = re.search(r"\[DOCK64\] start icon src=(\S+) size=(\d+) ok=(\d)", log)
        check("[DOCK64] start icon 打点", si is not None, si.group(0) if si else "（无）")
        check("开始按钮 src=vfs:/logo/kaisi.png（从盘上真文件读，不是内置兜底）",
              si is not None and si.group(1) == "vfs:/logo/kaisi.png" and si.group(3) == "1",
              si.group(0) if si else "（无）")
        check("真文件安装证据（[IMG64] install path=/logo/kaisi.png）",
              "[IMG64] install path=/logo/kaisi.png" in log,
              (re.search(r"\[IMG64\] install path=/logo/kaisi\.png[^\r\n]*", log) or ["（无）"])[0])

        print("=== 3) 桌面基线帧（Dock/桌面图标像素）===")
        time.sleep(0.8)
        base = os.path.join(tmp, "desktop.ppm")
        got = mon.shot(base)
        check("拿到桌面截图", got and os.path.exists(base))
        if not got:
            return boom()
        w, h, px = smt.read_ppm(base)

        di = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) gap=(\d+)", log)
        icon_px = int(di.group(6)) if di else 46
        it0 = re.search(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=(\d+) y=(\d+) w=\d+ h=\d+ "
                        r"cx=(\d+) cy=(\d+)", log)
        check("[DOCK64] item idx=0（开始按钮几何）打点", it0 is not None, it0.group(0) if it0 else "（无）")
        dock_xy = {}
        for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=\d+ name=\S+ x=\d+ y=\d+ w=(\d+) h=\d+ "
                             r"cx=(\d+) cy=(\d+)", log):
            dock_xy[int(m.group(1))] = (int(m.group(3)), int(m.group(4)), int(m.group(2)))
        if it0:
            cx, cy = int(it0.group(3)), int(it0.group(4))
            m_shot = ink_mask(px, w, cx - icon_px // 2, cy - icon_px // 2, icon_px, 60)
            v = iou(m_shot, host_mask(KAISI, icon_px))
            check("开始按钮区域形状与 logo/kaisi.png 一致（IoU>=0.55）", v >= 0.55,
                  "IoU=%.3f ink=%d" % (v, ink_count(m_shot)))

        print("=== 4) ① 桌面应用图标 = 图标包里的真图标（颜色 = 调色板亮色）===")
        for m in re.finditer(r"\[DESK64\] item idx=(\d+) kind=(\d+) name=\S+ x=(\d+) y=(\d+) w=(\d+)", log):
            kind, x, y = int(m.group(2)), int(m.group(3)), int(m.group(4))
            if kind > 2:
                continue
            name = ["mypc", "recycle", "terminal"][kind]
            rgb = APP_COLOR[name]
            other = APP_COLOR["tmgr"]
            r1 = color_ratio(px, w, x + 4, y + 4, 40, rgb)
            r2 = color_ratio(px, w, x + 4, y + 4, 40, other)
            check("桌面图标 #%d(%s) 区域含调色板亮色 #%02X%02X%02X（>=8%% 且高于其它色）" % (kind, name, *rgb),
                  r1 >= 0.08 and r1 > r2, "命中=%.2f 其它色=%.2f" % (r1, r2))

        print("=== 5) ① Dock 应用图标 = 真图标（改前是「渐变底 + 首字母」）===")
        dock_names = {4: "calc", 5: "mines", 6: "settings", 7: "tmgr", 8: "monitor-app"}
        for idx, name in sorted(dock_names.items()):
            if idx not in dock_xy:
                check("Dock #%d(%s) 几何打点" % (idx, name), False, "（无）")
                continue
            cx, cy, n = dock_xy[idx]
            rgb = APP_COLOR[name]
            other = APP_COLOR["mines"] if name != "mines" else APP_COLOR["tmgr"]
            r1 = color_ratio(px, w, cx - n // 2, cy - n // 2, n, rgb)
            r2 = color_ratio(px, w, cx - n // 2, cy - n // 2, n, other)
            check("Dock #%d(%s) 图标区域含调色板亮色（>=6%% 且高于其它色）" % (idx, name),
                  r1 >= 0.06 and r1 > r2, "命中=%.2f 其它色=%.2f" % (r1, r2))

        print("=== 6) ①+④ 开始菜单状态区图标（形状 IoU + 主题跟随）===")
        n_before = len(vm.log())
        opened = False
        if it0:
            cur = smt.Cursor()
            for _ in range(3):
                cur.goto(mon, int(it0.group(3)), int(it0.group(4)))
                time.sleep(0.35)
                mon.click()
                if vm.wait_log("[START64] open why=start-button", 6, since=n_before):
                    opened = True
                    break
        check("点 Dock 开始按钮打开开始菜单（[START64] open why=start-button）", opened,
              (re.search(r"\[START64\] open why=[^\r\n]*", vm.log()[n_before:]) or ["（无）"])[0])
        log2 = vm.log()
        st_net = re.search(r"\[START64\] status icon net x=(\d+) y=(\d+) kind=(\w+)", log2)
        st_snd = re.search(r"\[START64\] status icon sound x=(\d+) y=(\d+)", log2)
        st_ntf = re.search(r"\[START64\] status icon notif x=(\d+) y=(\d+)", log2)
        check("状态区三个图标几何打点（net/sound/notif）",
              st_net is not None and st_snd is not None and st_ntf is not None,
              (re.search(r"\[START64\] status icon (net|sound|lang|notif)[^\r\n]*",
                         vm.log()[n_before:]) or ["（无）"])[0])
        check("网络图标在 QEMU 里是有线（kind=ethernet）",
              st_net is not None and st_net.group(3) == "ethernet",
              st_net.group(0) if st_net else "（无）")
        time.sleep(1.2)
        sh1 = os.path.join(tmp, "menu_light.ppm")
        mon.shot(sh1)
        w1, h1, px1 = smt.read_ppm(sh1)
        for name, mm in (("ethernet", st_net), ("volume", st_snd), ("bell", st_ntf)):
            if mm is None:
                check("状态区 %s 图标形状 IoU" % name, False, "（没拿到几何）")
                continue
            x, y = int(mm.group(1)), int(mm.group(2))
            m_shot = ink_mask(px1, w1, x, y, SM_ICON, 60)
            m_host = host_mask(os.path.join(ICON_DIR, "system", "%s@24.png" % name), SM_ICON)
            v = iou(m_shot, m_host)
            check("状态区 %s 图标形状与宿主 PNG 一致（IoU>=0.55）" % name, v >= 0.55,
                  "IoU=%.3f ink=%d/%d" % (v, ink_count(m_shot), ink_count(m_host)))

        print("=== 7) ④ 切暗色主题 -> 同一图标形状不变、颜色跟随 Token 变浅 ===")
        before = len(vm.log())
        mon.key("ctrl-shift-t", wait=1.2)
        got_t = vm.wait_log("[THEME64] apply theme=1", 30, since=before)
        if not got_t:
            mon.key("ctrl-shift-t", wait=1.2)
            got_t = vm.wait_log("[THEME64] apply theme=1", 30, since=before)
        ap = re.search(r"\[THEME64\] apply theme=1 name=\S+ dark=1 accent=#\w{6}", vm.log()[before:])
        check("切到暗色主题（[THEME64] apply theme=1 dark=1）", got_t and ap is not None,
              ap.group(0) if ap else "（无）")
        time.sleep(1.2)
        sh2 = os.path.join(tmp, "menu_dark.ppm")
        mon.shot(sh2)
        w2, h2, px2 = smt.read_ppm(sh2)
        if st_snd is not None:
            x, y = int(st_snd.group(1)), int(st_snd.group(2))
            c1, n1 = ink_color(px1, w1, x, y, SM_ICON)
            c2, n2 = ink_color(px2, w2, x, y, SM_ICON)
            l1 = sum(c1) / 3.0 if c1 else 0
            l2 = sum(c2) / 3.0 if c2 else 0
            check("状态区声音图标颜色随主题变（白主题偏深、暗主题变浅，亮度差 >=60）",
                  c1 is not None and c2 is not None and l2 - l1 >= 60,
                  "白主题墨=%s(亮度%.0f) 暗主题墨=%s(亮度%.0f)" % (c1, l1, c2, l2))
            m_shot2 = ink_mask(px2, w2, x, y, SM_ICON, 60)
            m_host = host_mask(os.path.join(ICON_DIR, "system", "volume@24.png"), SM_ICON)
            v2 = iou(m_shot2, m_host)
            check("暗色主题下同一图标形状仍与 PNG 一致（IoU>=0.5，只换色不换形）", v2 >= 0.5,
                  "IoU=%.3f ink=%d" % (v2, ink_count(m_shot2)))
            check("暗色主题下墨迹像素存在（没被主题切换画丢）", n2 >= 8, "ink=%d" % n2)
    finally:
        vm.close()

    # =============================================================
    print("=== 8) ②(a) 改坏 terminal 三个档 + mypc@48 -> 精确回落 + 可降级 ===")
    img2 = make_fixture(tmp, "icons_crc.img", corrupt="crc")
    vm2 = smt.Vm(qemu, img2, args.port + 221, "vimtu-icons64-crc", tmp)
    try:
        mon2 = vm2.wait_monitor()
        # ★ 缺陷 4（登录必须显式输入）：锁屏可交互 -> 回车两次进桌面（同 startmenu64_test）。
        vm2.wait_log("[LOCK64] bg blur ready", 150)
        mon2.key("ret", wait=1.0)
        mon2.key("ret", wait=1.5)
        up2 = vm2.wait_log("[GUI64] ready", 180)
        check("坏包盘仍能进桌面（[GUI64] ready）", up2)
        l2 = vm2.log()
        init2 = re.search(r"\[ICON64\] init pack lba=\d+ drive=\d+ bytes=\d+ entries=\d+ icons=\d+ "
                          r"bad=(\d+) ok=(\d)", l2)
        check("改了 4 个条目 -> bad=4 ok=0（如实打点，不假装成功）",
              init2 is not None and init2.group(1) == "4" and init2.group(2) == "0",
              init2.group(0) if init2 else "（无）")
        fb = re.search(r"\[ICON64\] fallback kind=terminal reason=(\S+) \(programmatic draw kept\)", l2)
        check("terminal 三个档全坏 -> 回落并打点（[ICON64] fallback kind=terminal reason=…）",
              fb is not None, fb.group(0) if fb else "（无）")
        check("mypc 只坏大档 -> 自动降级用 32px 档（[ICON64] load kind=mypc path=pack:apps/mypc@32.png）",
              re.search(r"\[ICON64\] load kind=mypc path=pack:apps/mypc@32\.png size=32 src=pack ok=1",
                        l2) is not None,
              (re.search(r"\[ICON64\] load kind=mypc[^\r\n]*", l2) or ["（无）"])[0])
        check("其它图标照常从包里加载（[ICON64] load kind=volume … src=pack ok=1）",
              re.search(r"\[ICON64\] load kind=volume path=\S+ size=\d+ src=pack ok=1", l2) is not None)
        time.sleep(0.8)
        shot2 = os.path.join(tmp, "crc_desktop.ppm")
        try:
            mon2.shot(shot2)
            w3, h3, px3 = smt.read_ppm(shot2)
        except Exception as e:                       # 拿不到帧就如实报失败，不崩在 socket 上
            print("  （坏包盘截图失败：%s）" % e)
            w3 = h3 = px3 = None
        it3 = None
        for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=\d+ name=\S+ x=\d+ y=\d+ w=(\d+) h=\d+ "
                             r"cx=(\d+) cy=(\d+)", l2):
            if int(m.group(1)) == 3:
                it3 = m
        if it3 and px3 is not None:
            n = int(it3.group(2))
            x0, y0 = int(it3.group(3)) - n // 2, int(it3.group(4)) - n // 2
            m_shot = ink_mask(px3, w3, x0, y0, n, 60)
            check("终端 Dock 图标区域仍有墨迹（回落成内嵌程序化图标，界面不空）", ink_count(m_shot) >= 60,
                  "ink=%d/%d" % (ink_count(m_shot), n * n))
        else:
            check("终端 Dock 图标几何打点（[DOCK64] item idx=3）", False, "（无）")
        it1 = None
        for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=\d+ name=\S+ x=\d+ y=\d+ w=(\d+) h=\d+ "
                             r"cx=(\d+) cy=(\d+)", l2):
            if int(m.group(1)) == 1:
                it1 = m
        if it1 and px3 is not None:
            n = int(it1.group(2))
            rgb = APP_COLOR["mypc"]
            r1 = color_ratio(px3, w3, int(it1.group(3)) - n // 2, int(it1.group(4)) - n // 2, n, rgb)
            check("mypc 降级到 32px 档后仍是真图标（区域含调色板亮色 >=6%%）", r1 >= 0.06, "命中=%.2f" % r1)
        check("开始按钮仍然画出（[DOCK64] start icon … ok=1）",
              re.search(r"\[DOCK64\] start icon src=\S+ size=\d+ ok=1", l2) is not None)
    finally:
        vm2.close()

    # =============================================================
    print("=== 9) ②(b) 包 magic 清零 -> 整包不可用、全部回落程序化绘制，界面照样不空 ===")
    img3 = make_fixture(tmp, "icons_nopack.img", corrupt="magic")
    vm3 = smt.Vm(qemu, img3, args.port + 22, "vimtu-icons64-nopack", tmp)
    try:
        mon3 = vm3.wait_monitor()
        # ★ 缺陷 4（登录必须显式输入）：锁屏可交互 -> 回车两次进桌面（同 startmenu64_test）。
        vm3.wait_log("[LOCK64] bg blur ready", 150)
        mon3.key("ret", wait=1.0)
        mon3.key("ret", wait=1.5)
        up3 = vm3.wait_log("[GUI64] ready", 180)
        check("无包盘仍能进桌面（[GUI64] ready）", up3)
        l3 = vm3.log()
        check("[ICON64] init pack absent（如实打点：包读不到）",
              re.search(r"\[ICON64\] init pack absent reason=\S+ lba=\d+", l3) is not None,
              (re.search(r"\[ICON64\] init pack absent[^\r\n]*", l3) or ["（无）"])[0])
        nfb = len(re.findall(r"\[ICON64\] fallback kind=\S+ reason=no-pack", l3))
        check("每个用到的 kind 都打回落点（[ICON64] fallback … reason=no-pack >= 8 条）", nfb >= 8, "%d 条" % nfb)
        check("selftest 没有包时也 PASS（回落路径可用，不算失败）",
              re.search(r"\[ICON64\] selftest PASS mask=0 pack=0", l3) is not None,
              (re.search(r"\[ICON64\] selftest[^\r\n]*", l3) or ["（无）"])[0])
        time.sleep(0.8)
        it0b = re.search(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=\d+ y=\d+ w=\d+ h=\d+ "
                         r"cx=(\d+) cy=(\d+)", l3)
        w4 = h4 = px4 = None
        if it0b:
            cur3 = smt.Cursor()
            n_b = len(vm3.log())
            for _ in range(3):
                cur3.goto(mon3, int(it0b.group(1)), int(it0b.group(2)))
                time.sleep(0.35)
                mon3.click()
                if vm3.wait_log("[START64] open why=start-button", 6, since=n_b):
                    break
        time.sleep(1.2)
        shot3 = os.path.join(tmp, "nopack_menu.ppm")
        try:
            mon3.shot(shot3)
            w4, h4, px4 = smt.read_ppm(shot3)
        except Exception as e:
            print("  （无包盘截图失败：%s）" % e)
            w4 = h4 = px4 = None
        stn = re.search(r"\[START64\] status icon net x=(\d+) y=(\d+)", vm3.log())
        if stn and px4 is not None:
            x, y = int(stn.group(1)), int(stn.group(2))
            m_shot = ink_mask(px4, w4, x, y, SM_ICON, 60)
            check("没有包时状态区网络图标仍有墨迹（程序化线性图标兜底，界面不空）", ink_count(m_shot) >= 20,
                  "ink=%d/484" % ink_count(m_shot))
        else:
            check("状态区图标几何打点（[START64] status icon net）", False, "（无）")
        it1b = None
        for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=\d+ name=\S+ x=\d+ y=\d+ w=(\d+) h=\d+ "
                             r"cx=(\d+) cy=(\d+)", vm3.log()):
            if int(m.group(1)) == 1:
                it1b = m
        if it1b and px4 is not None:
            n = int(it1b.group(2))
            x0, y0 = int(it1b.group(3)) - n // 2, int(it1b.group(4)) - n // 2
            m_shot = ink_mask(px4, w4, x0, y0, n, 60)
            check("没有包时 Dock 我的电脑图标仍有墨迹（内嵌程序化图标兜底）", ink_count(m_shot) >= 60,
                  "ink=%d/%d" % (ink_count(m_shot), n * n))
        check("Dock 开始按钮照常（[DOCK64] start icon … ok=1）",
              re.search(r"\[DOCK64\] start icon src=\S+ size=\d+ ok=1", vm3.log()) is not None)
    finally:
        vm3.close()

    bad = [n for n, ok in checks if not ok]
    print("\n===== 汇总：%d 项断言，失败 %d 项 =====" % (len(checks), len(bad)))
    for n in bad:
        print("  FAIL: %s" % n)
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("临时目录保留：%s" % tmp)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
