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
PART_MAIN_LBA = 8009
SECTOR = 512
checks = []


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


def static_checks(img_path):
    TP, vol, inodes, total = read_vol(img_path)
    roots = {nm: (i, rec) for i, nm, rec in TP._entries(vol, 0, inodes)}
    check("根目录里有 /Wallpapers（预设壁纸）", "Wallpapers" in roots)
    check("根目录里有 /Avatars（预设头像）", "Avatars" in roots)
    if "Wallpapers" not in roots or "Avatars" not in roots:
        return None, None
    wi = roots["Wallpapers"][0]
    ai = roots["Avatars"][0]
    check("两个目录是**两个不同的 inode**（壁纸与头像分开、不混放）", wi != ai,
          "Wallpapers inode=%d / Avatars inode=%d" % (wi, ai))

    walls = TP._entries(vol, wi, inodes)
    avas = TP._entries(vol, ai, inodes)
    check("预设壁纸 >= 3 张（实测 %d）" % len(walls), len(walls) >= 3,
          " | ".join(n for _, n, _ in walls))
    check("预设头像 >= 6 张（实测 %d）" % len(avas), len(avas) >= 6,
          " | ".join(n for _, n, _ in avas))

    bad = []
    for i, nm, rec in walls:
        blob = TP._read_file(vol, rec, total)
        sz = png_size(blob)
        if not sz or sz[0] < 640 or sz[1] < 480:
            bad.append("%s(png=%s)" % (nm, sz))
            continue
        p = os.path.join(BUILD, "presets", nm)
        if os.path.exists(p) and open(p, "rb").read() != blob:
            bad.append("%s(卷里字节 != build64/presets)" % nm)
    check("每张壁纸：合法 PNG、>=640x480、与 build64/presets 逐字节一致", not bad,
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

    # ④ 运行时：把壁纸指到 /Wallpapers 里的第一张
    wall_name = walls[0][1]
    wall_path = "/Wallpapers/" + wall_name
    TP, vol, inodes, total = read_vol(img)
    expect_sz = None
    for i, nm, rec in TP._entries(vol, ids[0], inodes):
        if nm == wall_name:
            expect_sz = png_size(TP._read_file(vol, rec, total))
            break
    ok_set = set_wall_path(qemu, img, 5695, "setwall", wall_path)
    check("把 ui.wall.path 设成 %s 并落盘（cfg save ok）" % wall_path, ok_set)
    log2 = run_boot(qemu, img, 5696, "bootwall")
    # ★ 判据对齐（实测）：解码**成功**时内核打的是
    #     [IMG64] load path=/Wallpapers/xxx ok=1 bytes=… fmt=png 1024x768 (from VimtuFS2 system volume)
    #   而 `[GFX64] init … src=…` 只在**回落到内置壁纸**的分支打。所以正向断言 IMG64 那条，
    #   并断言**没有** builtin 回落（两条合起来才说明"用户选的图真的上屏了"）。
    m_img = re.search(r"\[IMG64\] load path=%s ok=1 bytes=(\d+) fmt=png (\d+)x(\d+)"
                      % re.escape(wall_path), log2)
    check("重启后内核真的解码并加载了这张壁纸（[IMG64] load … ok=1）",
          m_img is not None, m_img.group(0)[:120] if m_img else "（缺 [IMG64] load 行）")
    if expect_sz and m_img:
        check("解码尺寸 = 壁纸真实尺寸（%dx%d）" % (expect_sz[0], expect_sz[1]),
              int(m_img.group(2)) == expect_sz[0] and int(m_img.group(3)) == expect_sz[1],
              "%sx%s" % (m_img.group(2), m_img.group(3)))
    else:
        check("解码尺寸 = 壁纸真实尺寸", False)
    m_fb = re.search(r"\[GFX64\] init surface=\d+x\d+ wall=(\d+)x(\d+) src=(\S+)", log2)
    check("没有回落到内置壁纸（不出现 src=builtin 的兜底行）",
          not (m_fb and "builtin" in m_fb.group(3)),
          m_fb.group(0) if m_fb else "（无兜底行 = 正确）")
    for b in ("PANIC", "TRIPLE FAULT"):
        check("壁纸用例：无 %s" % b, b not in log2)

    # ⑤ 反向：不存在的路径 -> 回落内置壁纸
    ok_set2 = set_wall_path(qemu, img, 5697, "setbad", "/Wallpapers/does-not-exist.png")
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
