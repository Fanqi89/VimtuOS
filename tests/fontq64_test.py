# -*- coding: utf-8 -*-
"""tests/fontq64_test.py —— UI 字体质量（笔画加厚 + 对比度）+ 最小 fontconfig **可配置**验收。

用户原话："优化字体显示不平滑、不够清晰、颜色过浅、渲染不够好（除终端外）"、
"引入 fontconfig 来管理字体"。本测试只认两样东西：**串口打点** 与 **屏幕像素**。

v0.4.9 变了什么（判据变强，不是放宽）：
  * 夹具从 `build64/system.img`（**没有 VimtuFS2 卷**的裸载荷）换成从 `build64/sysdisk.img`
    复制的带卷盘：只有带卷，`/etc/fonts.conf` 才是"盘上的真文件"，`[FONTCFG] applied src=`
    才有资格要求 `file`。旧版拿不到卷，只能接受 `default(seed-denied)`。
  * `src=` 的断言从"file 或 default(...) 二选一"钉成 **必须 file** —— 这是"fontconfig 真的
    在管字体"的分水岭：读到盘上的配置 vs 退化用内置默认。
  * 新增 **A/B 对照**：同一底座造两份夹具，只把 `/etc/fonts.conf` 的 `ui.stem` 从 120 改成
    160，各启动一次 —— 第二次必须打 `stem=160`，并且同一块设置页客户区的"笔画核心"像素
    **方向性变多**（笔画更实）。只改一行配置就有可测的渲染差别，才叫"可配置"。

判据：
  1) fontconfig 生效：`[FONTCFG] load … ui_stem=120 …` / `[FONTCFG] applied src=file keys=N`
  2) UI 正文**对比度**（WCAG 相对亮度比）：最深正文像素 vs 卡片底色 >= 12:1（AAA 门槛是 7:1）
  3) UI 次要文字（text_dim）对比度 >= 5.5:1（84,88,96 一档的量化判据）
  4) 墨迹（笔画）充足：设置页正文区存在"笔画核心"像素（亮度 < 32）>= 0.2%
  5) 终端**不受影响**：同一帧里终端区域仍然只有黑/白两种颜色（三无原则没被 UI 的 AA 改动污染）
  6) A/B：stem=160 那份的 `applied stem=160`，且同区"笔画核心占比" > stem=120 那份
用法：py -3 tests/fontq64_test.py
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU / 构建产物缺失）。
"""
import argparse
import importlib.util
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build64")
SYS_DISK = os.path.join(BUILD, "sysdisk.img")        # 带 VimtuFS2 卷的完整演示盘（夹具底座）
PART_MAIN_LBA = 8009
SECTOR = 512
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "fontq")
os.makedirs(outdir, exist_ok=True)
checks = []

FONTS_CONF_TMPL = (
    "# VimtuOS fontconfig (minimal) - 夹具版（只改 ui.stem 做 A/B）\n"
    "ui.family       = Noto Sans\n"
    "ui.stem         = %d\n"
    "ui.antialias    = on\n"
    "mono.family     = Sarasa Mono SC\n"
    "mono.mode       = mono\n"
    "cjk.family      = Noto Sans SC\n"
    "fallback.family = Unifont\n"
)


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def lum(c):
    """WCAG 相对亮度（sRGB 0..255 -> 0..1）"""
    def f(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2])


def contrast(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_fixture(base_img, out_img, stem):
    """底座盘副本 + 卷里 /etc/fonts.conf 换成指定 ui.stem（就地替换卷，长度不变）。

    为什么要就地改卷而不是用 build_disk 重拼：本仓库的 sysdisk.img = system.img 前 8009 扇区
    + 主分区卷，重拼要保证两边字节对齐；就地替换卷字节（长度取自超级块）改动面最小。
    """
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = bytearray(open(base_img, "rb").read())
    off = PART_MAIN_LBA * SECTOR
    if bytes(d[off:off + 8]) != b"VIMTUFS2":
        raise SystemExit("底座盘的主分区不是 VimtuFS2 卷：%s" % base_img)
    total = int.from_bytes(bytes(d[off + 20:off + 24]), "little")
    vol_len = total * SECTOR
    ed = TP.VolumeEdit(total)
    ed.load(bytes(d[off:off + vol_len]))
    body = (FONTS_CONF_TMPL % stem).encode("utf-8")
    ed.put("/etc/fonts.conf", body, mode=0o666)
    out = ed.finish()
    if len(out) != vol_len:
        raise SystemExit("夹具卷长度变了：%d != %d" % (len(out), vol_len))
    d[off:off + vol_len] = out
    with open(out_img, "wb") as f:
        f.write(bytes(d))
    return out_img


def guest_fonts_conf(img):
    """宿主侧独立解析卷里的 /etc/fonts.conf（证明夹具真的落盘了，而不是内存里改改）。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img, "rb").read()
    off = PART_MAIN_LBA * SECTOR
    total = int.from_bytes(d[off + 20:off + 24], "little")
    vol = d[off:off + total * SECTOR]
    inodes = int.from_bytes(vol[40:44], "little")
    cur = 0
    rec = None
    for part in ("etc", "fonts.conf"):
        hit = None
        for i, nm, r in TP._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        cur, rec = hit
    return TP._read_file(vol, rec, total)


def sample_settings(mon, vm, outdir_name):
    """打开设置窗口并采样正文区：返回 (指标 dict) 或 None。"""
    mon.key("meta_l", 1.2)
    mon.key("6", 2.6)                      # 数字快捷键 6 = 设置
    vm.wait_log("[UI] win geom", 25)
    time.sleep(2.0)
    m = None
    for mm in re.finditer(r"\[UI\] win geom tag=(\S+) title=(.*?) app=5 x=(-?\d+) y=(-?\d+) "
                          r"w=(\d+) h=(\d+) client=(\d+)x(\d+)", vm.log()):
        m = mm
    if not m:
        return None
    cx, cy = int(m.group(3)) + 1, int(m.group(4)) + 25
    cw, ch = int(m.group(7)), int(m.group(8))
    f = mon.shot(os.path.join(outdir, outdir_name))
    a = T.frame_arr(f)
    x0, y0 = cx + cw // 2, cy + 60
    x1, y1 = cx + cw - 30, cy + ch - 60
    hist = {}
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            px = a[y][x]
            v = (int(px[0]), int(px[1]), int(px[2]))
            hist[v] = hist.get(v, 0) + 1
    if not hist:
        return None
    bg = max(hist.items(), key=lambda kv: kv[1])[0]
    darkest = min(hist.keys(), key=lambda v: sum(v))
    mid = [v for v in hist if 60 <= sum(v) / 3.0 <= 150]
    total = sum(hist.values())
    dark_cnt = sum(n for v, n in hist.items() if sum(v) / 3.0 < 96)
    core_cnt = sum(n for v, n in hist.items() if sum(v) / 3.0 < 32)
    return {
        "bg": bg, "darkest": darkest, "total": total,
        "c_main": contrast(darkest, bg),
        "c_mid": contrast(min(mid, key=lambda v: sum(v)), bg) if mid else 0.0,
        "dark_pct": 100.0 * dark_cnt / total,
        "core_pct": 100.0 * core_cnt / total,
    }


def boot_to_desktop(qemu, img, port, tag, label):
    vm = T.Vm(qemu, img, port, tag)
    mon = vm.monitor()
    if not mon:
        check("连上 monitor（%s）" % label, False)
        vm.close()
        return None, None
    if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
        check("等锁屏（%s）" % label, False)
        vm.close()
        return None, None
    mon.key("ret", 1.4)
    mon.key("ret", 1.4)
    if not vm.wait_log(T.DESKTOP_READY, 180):
        check("进桌面（%s）" % label, False)
        vm.close()
        return None, None
    time.sleep(2.0)
    return vm, mon


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK, help="带 VimtuFS2 卷的底座盘（默认 build64/sysdisk.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5673)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或底座盘不可用（%s）—— 先跑 bash build64.sh" % args.img)
        return 2

    print("=== fontq64_test：UI 字体质量 + 最小 fontconfig（可配置 A/B） ===")
    fx120 = os.path.join(outdir, "fontq_stem120.img")
    fx160 = os.path.join(outdir, "fontq_stem160.img")
    make_fixture(args.img, fx120, 120)
    make_fixture(args.img, fx160, 160)
    # 夹具自证：宿主侧独立解析卷字节，两份的 ui.stem 必须真的是 120 / 160
    c120 = guest_fonts_conf(fx120)
    c160 = guest_fonts_conf(fx160)
    st120 = re.search(r"ui\.stem\s*=\s*(\d+)", (c120 or b"").decode("utf-8", "replace"))
    st160 = re.search(r"ui\.stem\s*=\s*(\d+)", (c160 or b"").decode("utf-8", "replace"))
    check("夹具落盘：卷里 /etc/fonts.conf 的 ui.stem = 120 / 160（宿主侧独立解析）",
          bool(st120 and st160 and st120.group(1) == "120" and st160.group(1) == "160"),
          "A=%s B=%s bytes=%d/%d" % (st120.group(1) if st120 else "?",
                                     st160.group(1) if st160 else "?",
                                     len(c120 or b""), len(c160 or b"")))

    # ================= 阶段 A：stem=120（默认发行版配置） =================
    vm, mon = boot_to_desktop(qemu, fx120, args.port, "fontq120", "stem=120")
    if not vm:
        return 1
    metrics120 = None
    try:
        log = vm.log()
        m_load = re.search(r"\[FONTCFG\] load path=/etc/fonts\.conf ui_stem=(\d+) aa=(\d+)", log)
        m_app = re.search(r"\[FONTCFG\] applied src=(\S+) keys=(\d+) stem=(\d+) ui_aa=(\d+)", log)
        check("fontconfig 读到盘上的配置（[FONTCFG] load … ui_stem=120）", bool(m_load),
              m_load.group(0) if m_load else "（缺）")
        check("fontconfig 把策略落到字体层（applied keys/stem/ui_aa）", bool(m_app),
              m_app.group(0) if m_app else "（缺）")
        if m_app:
            # ★ 判据收紧（v0.4.9）：必须是盘上的文件，不接受 default(...)
            check("配置来源 = file（真读到 /etc/fonts.conf，不是内置默认）",
                  m_app.group(1) == "file", "src=%s" % m_app.group(1))
            check("笔画加厚为 120%（策略已落到字体层）", m_app.group(3) == "120",
                  "stem=%s keys=%s" % (m_app.group(3), m_app.group(2)))
        line_load = (re.search(r"\[FONTCFG\] load[^\r\n]*", log).group(0)
                     if re.search(r"\[FONTCFG\] load", log) else "")
        mapped = all(k in line_load for k in (" ui=", " mono=", " cjk=", " fb="))
        check("角色 -> 字体面映射可查询（ui/mono/cjk/fallback 四族齐全）", mapped,
              (line_load if line_load else "（缺 [FONTCFG] load 行）"))

        metrics120 = sample_settings(mon, vm, "settings120.ppm")
        if not metrics120:
            check("拿到设置窗口几何并采样", False)
            return 1
        print("  [info] A(stem=120) bg=%s darkest=%s 正文对比度=%.2f:1  次要文字对比度=%.2f:1  "
              "暗像素=%.2f%% 笔画核心=%.2f%%" % (metrics120["bg"], metrics120["darkest"],
                                                 metrics120["c_main"], metrics120["c_mid"],
                                                 metrics120["dark_pct"], metrics120["core_pct"]))
        check("UI 正文对比度 >= 12:1（WCAG AAA 门槛是 7:1）", metrics120["c_main"] >= 12.0,
              "%.2f:1" % metrics120["c_main"])
        check("UI 次要文字对比度 >= 5.5:1（\"颜色过浅\"这条的量化判据）", metrics120["c_mid"] >= 5.5,
              "%.2f:1" % metrics120["c_mid"])
        # 盘点不拍脑袋：“暗像素占比”只**如实报数”（不设阀值）—— 它与窗口里有多少文字直接相关
        print("  [info] 暗像素占比 = %.2f%%（仅报数：随窗口文字量变化）" % metrics120["dark_pct"])
        check("笔画核心存在（亮度 < 32 的像素 >= 0.2%）—— 细笔画常只有中间灰、没有核心",
              metrics120["core_pct"] >= 0.2, "%.2f%%" % metrics120["core_pct"])

        # 终端仍是三无（UI 的 AA 改动不应污染终端）
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        if vm.wait_log("[APP] term opened", 25):
            time.sleep(1.2)
            mt = None
            for mm in re.finditer(r"\[UI\] win geom tag=(\S+) title=(.*?) app=3 x=(-?\d+) y=(-?\d+) "
                                  r"w=(\d+) h=(\d+) client=(\d+)x(\d+)", vm.log()):
                mt = mm
            if mt:
                tx, ty = int(mt.group(3)) + 8, int(mt.group(4)) + 32
                tw, th = int(mt.group(7)) - 16, int(mt.group(8)) - 60
                ft = mon.shot(os.path.join(outdir, "term.ppm"))
                at = T.frame_arr(ft)
                gray = 0
                for y in range(ty, ty + th, 2):
                    for x in range(tx, tx + tw, 2):
                        px = at[y][x]
                        v = (int(px[0]), int(px[1]), int(px[2]))
                        if v in ((0, 0, 0), (255, 255, 255)):
                            continue
                        if max(v) - min(v) <= 1 and 0 < v[0] < 255:
                            gray += 1
                check("终端仍是无抗锯齿（中间灰 = 0；UI 的笔画加厚不影响终端）", gray == 0,
                      "灰阶像素=%d" % gray)
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s（stem=120）" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    # ================= 阶段 B：stem=160（只改一行配置） =================
    vm160, mon160 = boot_to_desktop(qemu, fx160, args.port + 1, "fontq160", "stem=160")
    if not vm160:
        return 1
    try:
        log = vm160.log()
        m_b = re.search(r"\[FONTCFG\] applied src=(\S+) keys=(\d+) stem=(\d+) ui_aa=(\d+)", log)
        check("B：只改 ui.stem=160 就生效（applied src=file stem=160）",
              bool(m_b and m_b.group(1) == "file" and m_b.group(3) == "160"),
              m_b.group(0) if m_b else "（缺）")
        metrics160 = sample_settings(mon160, vm160, "settings160.ppm")
        if not metrics160:
            check("B：拿到设置窗口几何并采样", False)
            return 1
        print("  [info] B(stem=160) bg=%s darkest=%s 正文对比度=%.2f:1  次要文字对比度=%.2f:1  "
              "暗像素=%.2f%% 笔画核心=%.2f%%" % (metrics160["bg"], metrics160["darkest"],
                                                 metrics160["c_main"], metrics160["c_mid"],
                                                 metrics160["dark_pct"], metrics160["core_pct"]))
        if metrics120:
            check("B：笔画核心占比**方向性**高于 A（同一块设置页客户区，只差 ui.stem）",
                  metrics160["core_pct"] > metrics120["core_pct"],
                  "A=%.2f%% -> B=%.2f%%（+%.2f%%，仅报数）"
                  % (metrics120["core_pct"], metrics160["core_pct"],
                     metrics160["core_pct"] - metrics120["core_pct"]))
        check("B：正文对比度不因加厚而变差（仍 >= 12:1）", metrics160["c_main"] >= 12.0,
              "%.2f:1" % metrics160["c_main"])
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s（stem=160）" % bad, bad not in vm160.log())
    finally:
        try:
            vm160.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d" % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
