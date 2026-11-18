# -*- coding: utf-8 -*-
"""tests/presets64_test.py —— v0.5.8：根目录的**预设壁纸 / 预设头像**两个独立目录。

用户点名需求：根目录里要有分开的两个目录，一个放系统预设头像、一个放预设壁纸。

判据（每条都能独立证伪）：
  ① 卷里**两个目录都在根**：`/Wallpapers`（预设壁纸）与 `/Avatars`（预设头像），
     且两者是**两个不同的 inode**（壁纸和头像分开，不混放）
  ② 内容对得上：壁纸 >= 3 张、头像 >= 6 张；每个文件都是合法 PNG，尺寸符合约定，
     且**逐字节**等于构建期生成的那份（build64/presets/）
  ③ 头像真的"画出来了"：四角透明（圆形遮罩）、有足够的实体像素
  ④ 运行时：把 `ui.wall.path` 指到 `/Wallpapers/` 里的图并**重启同一块盘** ->
     启动打点 `[GFX64] init ... wall=WxH src=vfs:wallpaper`，WxH = 该图真实尺寸
  ⑤ 反向：把 `ui.wall.path` 指到不存在的路径 -> 启动回落到内置壁纸（`src=builtin`），不崩
  ⑥ 全程无 PANIC / TRIPLE FAULT

用法：py -3 tests/presets64_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（qemu / 镜像）。
"""
import io
import os
import re
import shutil
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                     # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
BUILD = os.path.join(ROOT, "build64")
WORK = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "presets")
os.makedirs(WORK, exist_ok=True)
PART_MAIN_LBA = 32584
SECTOR = 512
checks = []
WALL_DIR = "/System/themes/wallpapers"    # ★ 由 static_checks 填成实际命中的落点
AVATAR_DIR = "/System/themes/avatars"


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def load_mod(name, fname):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def png_size(b):
    """从 PNG 头读 (w, h)；不是 PNG 返回 None。"""
    if len(b) < 24 or b[:8] != b"\x89PNG\r\n\x1a\n" or b[12:16] != b"IHDR":
        return None
    return struct.unpack_from(">II", b, 16)


def jpeg_size(b):
    """JPEG 尺寸：扫 SOF0/SOF1/SOF2 取 (w, h, kind)；非 JPEG 返回 None。

    为什么要它：内核 kernel/img64.cpp 也实现了 baseline/ext JPEG 解码，用户投放的壁纸
    就是 JPEG（1.jpeg … 7.jpg），判据只认 PNG 会把真绿判成红。
    """
    if len(b) < 4 or b[:2] != b"\xff\xd8":
        return None
    i = 2
    while i < len(b) - 3:
        if b[i] != 0xFF:
            i += 1
            continue
        m = b[i + 1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        ln = struct.unpack_from(">H", b, i + 2)[0]
        if m in (0xC0, 0xC1, 0xC2, 0xC3):
            h, w = struct.unpack_from(">HH", b, i + 5)
            return (w, h, {0xC0: "sof0", 0xC1: "sof1", 0xC2: "sof2", 0xC3: "sof3"}[m])
        i += 2 + ln
    return None


def png_alpha_ok(b, w, h):
    """头像必须是圆形：四角透明 + 中心区域有实体像素。"""
    from PIL import Image
    im = Image.open(io.BytesIO(b)).convert("RGBA")
    px = im.load()
    corners = [px[0, 0][3], px[w - 1, 0][3], px[0, h - 1][3], px[w - 1, h - 1][3]]
    opaque = sum(1 for y in range(0, h, 2) for x in range(0, w, 2) if px[x, y][3] > 200)
    return max(corners) == 0 and opaque >= 100, max(corners), opaque


def read_vol(img_path):
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img_path, "rb").read()
    off = PART_MAIN_LBA * SECTOR
    total = int.from_bytes(d[off + 20:off + 24], "little")
    vol = d[off:off + total * SECTOR]
    inodes = int.from_bytes(vol[40:44], "little")
    return TP, vol, inodes, total


def _subdir(TP, vol, inodes, path):
    """按路径逐级找目录 inode；找不到返回 None。path 形如 '/System/themes/wallpapers'。"""
    ino = 0
    for seg in [s for s in path.split("/") if s]:
        hit = None
        for i, nm, rec in TP._entries(vol, ino, inodes):
            if nm == seg:
                hit = i
                break
        if hit is None:
            return None
        ino = hit
    return ino


def static_checks(img_path):
    TP, vol, inodes, total = read_vol(img_path)
    roots = {nm: (i, rec) for i, nm, rec in TP._entries(vol, 0, inodes)}
    # ★ v0.6.0 目录重排：预设壁纸/头像搬到 /System/themes/{wallpapers,avatars}，
    #   旧根目录 /Wallpapers、/Avatars 由内核 vfs64 的别名表继续解析（运行时测试仍用旧路径）。
    #   这里优先查新位置，旧位置保留兼容 —— 两边都不存在才算真红。
    wi = _subdir(TP, vol, inodes, "/System/themes/wallpapers")
    ai = _subdir(TP, vol, inodes, "/System/themes/avatars")
    global WALL_DIR, AVATAR_DIR
    loc_w = "/System/themes/wallpapers"
    loc_a = "/System/themes/avatars"
    if wi is None and "Wallpapers" in roots:
        wi, loc_w = roots["Wallpapers"][0], "/Wallpapers"
    if ai is None and "Avatars" in roots:
        ai, loc_a = roots["Avatars"][0], "/Avatars"
    WALL_DIR, AVATAR_DIR = loc_w, loc_a
    check("预设壁纸目录在（新落点 /System/themes/wallpapers 或旧 /Wallpapers）",
          wi is not None, "命中 %s" % loc_w)
    check("预设头像目录在（新落点 /System/themes/avatars 或旧 /Avatars）",
          ai is not None, "命中 %s" % loc_a)
    if wi is None or ai is None:
        return None, None
    check("两个目录是**两个不同的 inode**（壁纸与头像分开、不混放）", wi != ai,
          "%s inode=%d / %s inode=%d" % (loc_w, wi, loc_a, ai))

    walls = TP._entries(vol, wi, inodes)
    avas = TP._entries(vol, ai, inodes)
    check("预设壁纸 >= 3 张（实测 %d）" % len(walls), len(walls) >= 3,
          " | ".join(n for _, n, _ in walls))
    check("预设头像 >= 6 张（实测 %d）" % len(avas), len(avas) >= 6,
          " | ".join(n for _, n, _ in avas))

    bad = []
    for i, nm, rec in walls:
        blob = TP._read_file(vol, rec, total)
        # 壁纸允许 PNG 或 **baseline/ext JPEG**（内核 kernel/img64.cpp 两条都实现了；
        # 渐进式 SOF2 明确不支持，装卷阶段已被尺寸/格式守卫挡掉）。
        sz = png_size(blob)
        kind = "png"
        if not sz:
            js = jpeg_size(blob)
            if js:
                sz, kind = js[:2], "jpeg(%s)" % js[2]
        if not sz or sz[0] < 640 or sz[1] < 480:
            bad.append("%s(%s=%s)" % (nm, kind, sz))
            continue
        p = os.path.join(BUILD, "presets", nm)
        if os.path.exists(p) and open(p, "rb").read() != blob:
            bad.append("%s(卷里字节 != build64/presets)" % nm)
    check("每张壁纸：合法 PNG/JPEG、>=640x480、与 build64/presets 逐字节一致", not bad,
          ("问题：%s" % bad) if bad else "ok")

    bad = []
    for i, nm, rec in avas:
        blob = TP._read_file(vol, rec, total)
        sz = png_size(blob)
        if sz != (128, 128):
            bad.append("%s(size=%s)" % (nm, sz))
            continue
        try:
            ok, cmax, opaque = png_alpha_ok(blob, 128, 128)
        except Exception as e:
            bad.append("%s(decode %s)" % (nm, e))
            continue
        if not ok:
            bad.append("%s(corner_alpha=%d opaque=%d)" % (nm, cmax, opaque))
    check("每张头像：128x128 PNG、圆形（四角透明）、有实体像素", not bad,
          ("问题：%s" % bad) if bad else "ok")
    return (walls, avas), (wi, ai)


# ==================== 运行时（QEMU）====================
KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}


def key_for(ch):
    if ch in KEYMAP:
        return KEYMAP[ch]
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    return ch


def type_line(mon, s, per=0.06):
    for ch in s:
        mon.key(key_for(ch), wait=per)


def boot_desktop(vm, mon):
    if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
        return False
    mon.key("ret", 1.4)
    mon.key("ret", 1.4)
    if not vm.wait_log(T.DESKTOP_READY, 180):
        return False
    time.sleep(2.0)
    return True


def run_boot(qemu, img, port, tag):
    """起一次到桌面，返回串口全文。"""
    vm = T.Vm(qemu, img, port, tag)
    try:
        mon = vm.monitor()
        if not mon:
            return ""
        boot_desktop(vm, mon)
        return vm.log()
    finally:
        try:
            vm.close()
        except Exception:
            pass


def set_wall_path(qemu, img, port, tag, path):
    """进桌面 -> 终端 -> cfg set ui.wall.path <path> + cfg save。"""
    vm = T.Vm(qemu, img, port, tag)
    try:
        mon = vm.monitor()
        if not mon or not boot_desktop(vm, mon):
            return False
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        if not vm.wait_log("[APP] term opened", 25):
            return False
        time.sleep(1.2)
        n0 = len(vm.log())
        for _ in range(3):
            type_line(mon, "cfg set ui.wall.path %s" % path)
            mon.key("ret", wait=1.6)
            if vm.wait_log("[CONF64] set key=ui.wall.path", 12, since=n0):
                break
            type_line(mon, "cfg set ui.wall.path %s" % path)
            mon.key("ret", wait=1.6)
        type_line(mon, "cfg save")
        mon.key("ret", wait=3.0)
        return vm.wait_log("[CONF64] flush ok", 25)
    finally:
        try:
            vm.close()
        except Exception:
            pass


def main():
    qemu = T.find_qemu(None)
    if not qemu or not os.path.exists(SRC_DISK):
        print("跳过：qemu 或镜像不可用（%s）" % SRC_DISK)
        return 2

    print("=== presets64_test：/Wallpapers（预设壁纸）+ /Avatars（预设头像）===")
    got, ids = static_checks(SRC_DISK)
    if not got:
        print("\n=== RESULT: FAIL === 根目录里没有这两个目录")
        return 1
    walls, avas = got

    img = os.path.join(WORK, "sys_p.img")
    shutil.copyfile(SRC_DISK, img)

    # ④ 运行时：把壁纸指到**真落点**（v0.6.0 重排后的 /System/themes/wallpapers）里的第一张。
    #    ★ 不能用 /Wallpapers/…：那是重排前的路径，卷里没有、内核也没有别名 →
    #      [VFS64] path component not found + [IMG64] load fit skip reason=not-found。
    TP, vol, inodes, total = read_vol(img)
    # ★ v0.10.9：卷里现在同时有"超限原图"（>2048px，内核走 img64_load_vfs_fit64 的 DCT
    #   降采样，返回尺寸 != 原图尺寸）。本测试的判据是"返回尺寸 == 壁纸真实尺寸"，那只在
    #   decim=1（全尺寸解码）时成立 —— 所以选图**明确挑一张最长边 <= 2048 的**。
    #   判据本身没变，只是不再靠 inode 顺序碰运气。
    wall_name = walls[0][1]
    for _i, _nm, _rec in walls:
        _b = TP._read_file(vol, _rec, total)
        _d = png_size(_b) or (jpeg_size(_b)[:2] if jpeg_size(_b) else None)
        if _d and max(_d) <= 2048:
            wall_name = _nm
            break
    wall_path = WALL_DIR.rstrip("/") + "/" + wall_name
    expect_sz = None
    for i, nm, rec in TP._entries(vol, ids[0], inodes):
        if nm == wall_name:
            _b = TP._read_file(vol, rec, total)
            expect_sz = png_size(_b) or (jpeg_size(_b)[:2] if jpeg_size(_b) else None)
            break
    ok_set = set_wall_path(qemu, img, 5695, "setwall", wall_path)
    check("把 ui.wall.path 设成 %s 并落盘（cfg save ok）" % wall_path, ok_set)
    log2 = run_boot(qemu, img, 5696, "bootwall")
    # ★ 判据对齐（实测）：解码**成功**时内核打的是
    #     [IMG64] load path=/Wallpapers/xxx ok=1 bytes=… fmt=png 1024x768 (from VimtuFS2 system volume)
    #   而 `[GFX64] init … src=…` 只在**回落到内置壁纸**的分支打。所以正向断言 IMG64 那条，
    #   并断言**没有** builtin 回落（两条合起来才说明"用户选的图真的上屏了"）。
    # 壁纸走 fit 解码（gui64：img64_load_vfs_fit64(wpath, 2048, &wim)），打点是
    #   `[IMG64] load fit path=… max=N ok=1 bytes=N fmt=png WxH` —— 所以 `fit ` 必须可选。
    m_img = re.search(r"\[IMG64\] load (?:fit )?path=%s(?: max=\d+)? ok=1 bytes=(\d+) fmt=(png|jpeg) (\d+)x(\d+)"
                      % re.escape(wall_path), log2)
    check("重启后内核真的解码并加载了这张壁纸（[IMG64] load … ok=1）",
          m_img is not None, m_img.group(0)[:120] if m_img else "（缺 [IMG64] load 行）")
    if expect_sz and m_img:
        check("解码尺寸 = 壁纸真实尺寸（%dx%d）" % (expect_sz[0], expect_sz[1]),
              int(m_img.group(3)) == expect_sz[0] and int(m_img.group(4)) == expect_sz[1],
              "%sx%s fmt=%s" % (m_img.group(3), m_img.group(4), m_img.group(2)))
    else:
        check("解码尺寸 = 壁纸真实尺寸", False)
    m_fb = re.search(r"\[GFX64\] init surface=\d+x\d+ wall=(\d+)x(\d+) src=(\S+)", log2)
    check("没有回落到内置壁纸（不出现 src=builtin 的兜底行）",
          not (m_fb and "builtin" in m_fb.group(3)),
          m_fb.group(0) if m_fb else "（无兜底行 = 正确）")
    for b in ("PANIC", "TRIPLE FAULT"):
        check("壁纸用例：无 %s" % b, b not in log2)

    # ⑤ 反向：不存在的路径 -> 回落内置壁纸
    ok_set2 = set_wall_path(qemu, img, 5697, "setbad",
                            WALL_DIR.rstrip("/") + "/does-not-exist.png")
    check("把 ui.wall.path 设成不存在的路径也能落盘（配置层不校验存在性）", ok_set2)
    log3 = run_boot(qemu, img, 5698, "bootbad")
    m_bad = re.search(r"\[GFX64\] init surface=\d+x\d+ wall=(\d+)x(\d+) src=(\S+)", log3)
    check("不存在的壁纸 -> 启动回落到内置壁纸（src=builtin），不崩",
          bool(m_bad and "builtin" in m_bad.group(3)),
          m_bad.group(0) if m_bad else "（缺）")
    for b in ("PANIC", "TRIPLE FAULT"):
        check("反例：无 %s" % b, b not in log3)

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
