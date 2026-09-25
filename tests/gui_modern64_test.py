#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/gui_modern64_test.py - Windows 11 风格桌面（Dock + 主题 + 壁纸适应模式）验收

覆盖（每条都要串口 + 像素/输入证据）：
  1) Dock 几何        [DOCK64] geom/item：y = 屏高-76、高 60、水平居中、圆角 24、图标 44–48、间距 10–12；
                      像素：面板存在（与壁纸不同）、最左开始按钮是**真图标**（彩色像素）。
  2) 悬停放大/让位     鼠标闭环挪到某个图标上 -> [DOCK64] hover idx=.. scale=120 + 像素：
                      被悬停图标的顶边升高（放大 1.20），紧邻图标的左/右边界外移（轻微让位）。
  3) 点击回弹          点击 -> [DOCK64] bounce idx=.. frame=.. dy=.. 连续帧位移（向上再向下回弹）+
                      bounce done frames>=6（弹簧 260ms）。
  4) 最小化小横杠      Dock 图标 -> 最小化 -> [DOCK64] press action=minimize + [DOCK64] minbar ...
                      （宽 = 图标 40%、跟随强调色）+ 像素：横杠处有强调色像素；
                      并且**在横杠上点击不产生任何 Dock 动作**（纯视觉不可点击）。
  5) 主题切换（7 种）    Ctrl+Shift+T 循环：白 -> 暗 -> 蓝白 -> 粉白 -> 粉绿 -> 粉紫 -> 紫白 …；
                      每条 [THEME64] apply 打点 + 整屏像素差 > 阈值（暗色主题下 Dock 变深灰、主色变化）；
                      紫白额外断言强调色 #7C4DFF（打点 + Dock 小横杠像素）。
  6) 壁纸 6 种适应模式  Ctrl+Shift+N 循环 fill/fit/stretch/tile/center/span：
                      [GFX64] wall 几何（dst/scale/crop/tiles）+ 4 个定位标记的屏幕位置/裁剪 +
                      留白（无图区域方差≈0）/ 平铺接缝（x=1200 列 == x=0 列）/ 拉伸比例失真。
  7) 减少动画开关      Ctrl+Shift+R -> [THEME64] motion reduce=1 dock=0ms，点击后 bounce frames=1 motion=reduced。
  8) 开始按钮真图      [IMG64] install/load path=/logo/kaisi.png ok=1 + [IMG64] selftest real ok=1
                       + [DOCK64] start icon src=/logo/kaisi.png（或带 vfs: 前缀的同一条）；
                      宿主 Pillow 解 logo/kaisi.png（按内核 dock_rgba_from_img64 的 46x46 最近邻映射）与
                      截图里开始按钮逐像素比对；冷启动第二遍 reason=exists；再把盘上那份真图的第一块改坏，
                      验证「画出来的像素真的来自盘上文件」（load ok=0 -> 走 /kaisi.png 兜底）。
用法：python tests/gui_modern64_test.py [--img build64/system.img] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import struct
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

DOCK_H = 60
DOCK_MARGIN = 16
DOCK_R = 24
MARK_COLORS = {"r": (192, 48, 48), "g": (32, 160, 64), "b": (48, 64, 192), "y": (224, 192, 48)}

# ---- 各主题：Dock 面板/主色的期望（判"切到暗色后 Dock 变深灰"用）----
THEME_EXPECT = {
    0: ("white", 0),
    1: ("dark", 1),
    2: ("bluegrad", 0),
    3: ("pinkgrad", 0),
    4: ("pinkgreen", 0),
    5: ("pinkpurple", 0),
    6: ("purplegrad", 0),
}


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    import shutil
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM")
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


def near(c, t, tol=20):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))


def diff_count(a, b):
    n = 0
    for i in range(0, min(len(a), len(b)), 3):
        if a[i] != b[i] or a[i + 1] != b[i + 1] or a[i + 2] != b[i + 2]:
            n += 1
    return n


def dock_src_is_vfs(src):
    """[DOCK64] start icon src= 是否为"从 VimtuFS2 读到的真图"（不是内核内置兜底）。
    gui64 现在打的 src 是**裸路径**（/logo/kaisi.png、/kaisi.png）；早期文档写成 vfs:/... 前缀。
    两种形式都算真图；只有 builtin:icon_start.bin 是内置兜底。"""
    return isinstance(src, str) and (src.startswith("vfs:/") or src.startswith("/"))


# ==================== 真图验收用的宿主侧工具（Pillow + VimtuFS2 v3 直读）====================
PNG_LOGO = os.path.join(ROOT, "logo", "kaisi.png")
SECTOR = 512
V3_INODE_BYTES = 128


def png_render46(png_bytes, d=46):
    """Pillow 解 PNG，并按**内核 dock_rgba_from_img64 的映射**缩到 d x d：
    sx = x*w/dw、sy = y*h/dh（都是整型截断）。返回 (cells, (w,h))；没有 Pillow 时返回 (None, None)。
    cells 行优先，元素 = (r,g,b,a)——与内核喂给 blit_rgba_scaled 的 RGBA 缓冲逐字节对应。"""
    try:
        import io
        from PIL import Image
    except Exception:
        return None, None
    im = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    w, h = im.size
    px = im.load()
    cells = []
    for y in range(d):
        sy = y * h // d
        for x in range(d):
            cells.append(px[x * w // d, sy])
    return cells, (w, h)


def png_full_fnva(png_bytes):
    """宿主侧真文件基准：PIL 解**整张** PNG -> 内核同款 AARRGGBB 缓冲（小端）-> FNV-1a 32。
    没有 Pillow 时返回 None（调用方按"跳过指纹比对"处理）。"""
    try:
        import io
        import struct as _s
        from PIL import Image
    except Exception:
        return None
    try:
        data = Image.open(io.BytesIO(png_bytes)).convert("RGBA").tobytes()
    except Exception:
        return None
    buf = bytearray(len(data))
    for i in range(0, len(data), 4):
        r, g, b, a = data[i], data[i + 1], data[i + 2], data[i + 3]
        buf[i:i + 4] = _s.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    h = 2166136261
    for x in buf:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def wait_repaint64(vm, since, timeout=30):
    """等外壳按新主题/新模式**把整屏重画完**再截图（照 explorer64_test 的 settle 思路：只等状态，不放宽断言）。

    判据：`[GFX64] wall blur once ...` / `[GFX64] wall mode=...`（壁纸 + 模糊面重建完成，
    同一帧随后就画 Dock/窗口）。QEMU TCG 下这段重建要几秒（切主题尤其明显），
    固定 sleep 会抢在重画之前截图。"""
    wall = 0
    t0 = time.time()
    while time.time() - t0 < timeout:
        seg = vm.log()[since:]
        m = re.search(r"\[GFX64\] wall (blur once|mode=)", seg)
        if m:
            wall = m.end()
        # 判据 1：整屏重建已经开始；判据 2：之后再等到一行 [UI] clock text=（外壳每秒一行，
        # 说明这一帧真的画完了）——两个都要满足，避免"重建刚开始就截图"。
        if wall and re.search(r"\[UI\] clock text=", seg[wall:]):
            return True
        if vm.proc.poll() is not None:
            return False
        time.sleep(0.4)
    return False


def make_vfs_fixture(dst, sys_img):
    """16MB 夹具盘：build64/system.img 的字节 + 标准 MBR + @8009 的空 v3 卷。
    规则与 tests/fs_tree_test.py 的 make_small_system_disk 同一套（直接复用它的 _mbr_entry/vimtufs3_format），
    差别只是源镜像可以指定（--img）。返回 dict（多一个 bytes 字段）或 None。"""
    try:
        import fs_tree_test as fst
    except Exception:
        return None
    try:
        with open(sys_img, "rb") as f:
            sysb = f.read()
    except OSError:
        return None
    if not sysb or len(sysb) % SECTOR or len(sysb) > fst.SMALL_SECTORS * SECTOR:
        return None
    buf = bytearray(fst.SMALL_SECTORS * SECTOR)
    buf[0:len(sysb)] = sysb
    buf[446:462] = fst._mbr_entry(True, 0xEF, fst.PART_BOOT_LBA, fst.PART_BOOT_SECS)
    buf[462:478] = fst._mbr_entry(False, 0x07, fst.PART_MAIN_LBA, fst.SMALL_MAIN_SECTORS)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    info = fst.vimtufs3_format(buf, fst.PART_MAIN_LBA, fst.SMALL_MAIN_SECTORS)
    if info is None:
        return None
    info["main_lba"] = fst.PART_MAIN_LBA
    with open(dst, "wb") as f:
        f.write(buf)
    return info


def v3_find_inodes(buf, base_lba, name):
    """宿主侧在 v3 卷里找 name 的所有 inode（v3：名字在 inode 偏移 40、长度在偏移 1）。
    返回 [(inode 字节偏移, inode 号, parent)]；superblock 不对时返回 []。"""
    off = base_lba * SECTOR
    if buf[off:off + 8] != b"VIMTUFS2":
        return []
    inode_start = struct.unpack_from("<I", buf, off + 36)[0]     # inode 区起始块号
    inodes = struct.unpack_from("<I", buf, off + 40)[0]
    out = []
    for i in range(inodes):
        o = off + inode_start * SECTOR + i * V3_INODE_BYTES
        if buf[o] == 0 or buf[o + 1] != len(name):               # 空槽 / 名字长度不同
            continue
        if bytes(buf[o + 40:o + 40 + len(name)]) == name:
            out.append((o, i, struct.unpack_from("<I", buf, o + 28)[0]))
    return out


def v3_read_file(buf, vol_off, inode_off):
    """按 v3 映射读文件（4 个直接块 + 一级间接块）；返回 (size, bytes)。"""
    size = struct.unpack_from("<I", buf, inode_off + 4)[0]
    blocks = [struct.unpack_from("<I", buf, inode_off + 8 + 4 * d)[0] for d in range(4)]
    need = (size + SECTOR - 1) // SECTOR
    if need > 4:
        ind = struct.unpack_from("<I", buf, inode_off + 24)[0]
        for k in range(min(need - 4, 128)):
            blocks.append(struct.unpack_from("<I", buf, vol_off + ind * SECTOR + 4 * k)[0])
    out = bytearray()
    for b in blocks:
        out += buf[vol_off + b * SECTOR:vol_off + b * SECTOR + SECTOR]
        if len(out) >= size:
            break
    return size, bytes(out[:size])


def v3_corrupt_file(buf, vol_off, inode_off, n=64):
    """把文件第一块的前 n 字节写成 0xFF（大小不变）——用来证明"画出来的像素真的来自盘上文件"。"""
    blocks = [struct.unpack_from("<I", buf, inode_off + 8 + 4 * d)[0] for d in range(4)]
    p = vol_off + blocks[0] * SECTOR
    buf[p:p + n] = b"\xFF" * n
    return blocks[0]


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def raw(self, cmds, wait_between=0.12, wait_end=0.5):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            for c in cmds:
                s.sendall(c.encode() + b"\n")
                time.sleep(wait_between)
            time.sleep(wait_end)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def move(self, dx, dy, wait=0.3):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def button(self, val, wait=0.4):
        self.send("mouse_button %d" % val, wait=wait)

    def shot(self, path, wait=2.5):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


class Vm:
    def __init__(self, qemu, img, port, name="vimtu-modern", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_modern_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_ready(self, timeout=90):
        return self.wait_log("[GUI64] ready", timeout)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


# ==================== 鼠标闭环定位（移植自 tests/ui_extra64_test.py 的实测规律）====================
def probe_pos(vm, mon, wait_open=0.45):
    before = len(vm.log())
    mon.button(1, wait=wait_open)
    mon.button(0, wait=0.55)
    t0 = time.time()
    while time.time() - t0 < 2.5:
        log = vm.log()[before:]
        m = re.search(r"\[UI\] selbox x0=(\d+) y0=(\d+)", log)
        if m:
            return (int(m.group(1)), int(m.group(2)))
        if "[UI] desktop icon select kind=" in log or "[DOCK64] press" in log:
            return None
        time.sleep(0.25)
    return None


def _move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.move(mag if d > 0 else -mag, 0)
    else:
        mon.move(0, mag if d > 0 else -mag)


def aim_axis(mon, axis, d):
    if abs(d) < 20:
        return
    if abs(d) < 48:
        _move_axis(mon, axis, d, 14)
        return
    for _ in range((abs(d) + 23) // 24 - 1):
        _move_axis(mon, axis, d, 100)
    _move_axis(mon, axis, d, -20)


def goto(vm, mon, tx, ty, tries=10):
    """把光标闭环挪到 (tx,ty) 附近；返回最终探测到的位置（或 None，表示已压在图标上）。"""
    for _ in range(tries):
        pos = probe_pos(vm, mon)
        if pos is None:
            return None
        cx, cy = pos
        if abs(cx - tx) <= 20 and abs(cy - ty) <= 24:
            return pos
        aim_axis(mon, 0, tx - cx)
        aim_axis(mon, 1, ty - cy)
    return pos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5652)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_modern_")
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    vm = Vm(qemu, args.img, args.port, "vimtu-modern", tmp)
    try:
        mon = vm.wait_monitor()
        print("=== 1) Dock 几何（串口 + 像素）===")
        up = vm.wait_ready(90)
        check("桌面就绪", up)
        log = vm.log()
        g = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) gap=(\d+) items=(\d+) margin=(\d+) center=1 screen=(\d+)x(\d+)", log)
        check("[DOCK64] geom 打点", g is not None, g.group(0) if g else "（无）")
        if not up or g is None:
            print("  桌面没起来，跳过全部像素断言")
            raise SystemExit(1)
        dx, dy, dw, dh, dr = (int(g.group(i)) for i in range(1, 6))
        icon, gap, items = int(g.group(6)), int(g.group(7)), int(g.group(8))
        W, H = int(g.group(10)), int(g.group(11))
        check("Dock 高 60", dh == DOCK_H, "h=%d" % dh)
        check("Dock y = 屏高 - 76（离底 16px）", dy == H - DOCK_H - DOCK_MARGIN, "y=%d" % dy)
        check("Dock 水平居中（中心 = 屏宽/2）", abs((dx + dw // 2) - W // 2) <= 1,
              "中心 %d vs %d" % (dx + dw // 2, W // 2))
        check("Dock 圆角 24", dr == DOCK_R, "r=%d" % dr)
        check("图标 44-48px", 44 <= icon <= 48, "icon=%d" % icon)
        check("图标间距 10-12px", 10 <= gap <= 12, "gap=%d" % gap)
        clk = re.search(r"\[DOCK64\] clock chip x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log)
        dclk = (int(clk.group(1)), int(clk.group(3))) if clk else (W - DOCK_MARGIN - 210, 210)
        check("右下角时钟玻璃片几何打点", clk is not None, clk.group(0) if clk else "（无）")
        items_log = re.findall(r"\[DOCK64\] item idx=(\d+) app=(\d+) name=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log)
        check("每项都有几何打点（>= %d 项）" % items, len(items_log) >= items, "items=%d" % len(items_log))
        if items_log:
            i0 = items_log[0]
            check("最左是开始按钮（idx=0 且挨着面板左内边）",
                  int(i0[0]) == 0 and abs(int(i0[3]) - (dx + 9)) <= 2, "start x=%s dock_x=%d" % (i0[3], dx))
            i1 = items_log[1] if len(items_log) > 1 else None
            if i1:
                step = int(i1[3]) - int(i0[3])
                check("相邻图标步长 = 图标 + 间距", step == icon + gap, "step=%d" % step)

        shot0 = os.path.join(tmp, "base.ppm")
        mon.shot(shot0)
        w, h, px0 = read_ppm(shot0)
        check("分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
        gap_x = dx + 9 + icon + 5          # 图标 1 与图标 2 之间的间隙中点（纯面板）
        panel = sample(px0, w, gap_x, dy + dh // 2)
        above = sample(px0, w, gap_x, dy - 40)
        print("      面板色 %s / 面板上方壁纸 %s" % (panel, above))
        check("Dock 面板像素存在（面板与壁纸明显不同）",
              sum(abs(panel[i] - above[i]) for i in range(3)) > 6,
              "面板 %s 壁纸 %s" % (panel, above))
        # 开始按钮：真图标（彩色像素）
        scx = (int(i0[3]) + icon // 2, int(i0[4]) + icon // 2)
        colorful = 0
        for y in range(scx[1] - icon // 2, scx[1] + icon // 2):
            for x in range(scx[0] - icon // 2, scx[0] + icon // 2):
                c = sample(px0, w, x, y)
                if max(c) - min(c) > 40:
                    colorful += 1
        check("开始按钮用的是彩色真图标（logo/kaisi.png 像素）", colorful > 80, "彩色像素=%d" % colorful)

        # ---- 本段真图证据（默认的 build64/system.img 是**裸**镜像：没有 VimtuFS2 卷 -> 必须如实走内置兜底）----
        si = re.search(r"\[DOCK64\] start icon src=(\S+) size=(\d+) ok=(\d)", log)
        check("开始按钮打点（[DOCK64] start icon ...）", si is not None, si.group(0) if si else "（无）")
        bare = os.path.abspath(args.img) == os.path.abspath(os.path.join(ROOT, "build64", "system.img"))
        if bare:
            check("裸 system.img（无 VimtuFS2 卷）：开始按钮如实用内置兜底图（不假装从盘上读）",
                  si is not None and si.group(1) == "builtin:icon_start.bin" and si.group(3) == "1",
                  si.group(0) if si else "（无）")
            check("裸 system.img：真图 install/load 如实 skip（ok=0）",
                  "[IMG64] install skip path=/logo/kaisi.png reason=no-volume src=logo/kaisi.png" in log and
                  "[IMG64] load skip path=/logo/kaisi.png reason=not-found ok=0" in log,
                  (re.search(r"\[IMG64\] load skip path=/logo/kaisi\.png[^\r\n]*", log) or ["（无）"])[0])
        else:
            check("开始按钮 ok=1（内置兜底或 VimtuFS2 真图，--img 指定盘）",
                  si is not None and si.group(3) == "1" and
                  (si.group(1) == "builtin:icon_start.bin" or dock_src_is_vfs(si.group(1))),
                  si.group(0) if si else "（无）")
            check("真图来源可审计（install/load 打点都在）",
                  re.search(r"\[IMG64\] install (skip )?path=/logo/kaisi\.png", log) is not None and
                  re.search(r"\[IMG64\] load (skip )?path=/logo/kaisi\.png", log) is not None,
                  (re.search(r"\[IMG64\] load (skip )?path=/logo/kaisi\.png[^\r\n]*", log) or ["（无）"])[0])

        print("=== 2) 悬停放大 1.20 + 邻位让位（真实鼠标）===")
        # 先把光标挪到桌面空白处（顶边），再闭环挪到 idx=3（终端）图标中心
        mon.move(0, -1200, wait=0.5)
        HOVER_IDX = 4                       # 计算器：渐变方块图标（整块都是图标像素，放大/让位看得清）
        tgt = items_log[HOVER_IDX]
        tcx, tcy = int(tgt[7]), int(tgt[8])
        nb = items_log[HOVER_IDX - 1]       # 左邻
        ncx = int(nb[7])
        before_hover = len(vm.log())
        pos = goto(vm, mon, tcx, tcy)
        print("      图标中心 (%d,%d)，光标探测 %s" % (tcx, tcy, pos))
        hovered = vm.wait_log("[DOCK64] hover idx=%d" % HOVER_IDX, 6, since=before_hover)
        check("悬停打点（[DOCK64] hover idx=%d scale=120）" % HOVER_IDX, hovered)
        ms = re.search(r"\[DOCK64\] hover idx=%d scale=(\d+) neighbor_shift=(\d+)" % HOVER_IDX,
                       vm.log()[before_hover:])
        check("悬停放大 1.20（120%）且邻位让位 104%",
              ms is not None and ms.group(1) == "120" and ms.group(2) == "104",
              ms.group(0) if ms else "（无）")
        time.sleep(1.0)
        shot_h = os.path.join(tmp, "hover.ppm")
        mon.shot(shot_h)
        w2, h2, pxh = read_ppm(shot_h)

        def top_edge(px, cx):
            """从面板顶部往下扫，第一个与面板色明显不同的 y = 该列上图标顶边。
            （悬停放大后图标更高 → 顶边更靠上 = y 更小）"""
            for y in range(dy + 2, dy + dh - 2):
                c = sample(px, w, cx, y)
                if sum(abs(c[i] - panel[i]) for i in range(3)) > 40:
                    return y
            return dy + dh - 2

        base_top = top_edge(px0, tcx)
        hov_top = top_edge(pxh, tcx)
        print("      被悬停图标顶边：基线 %d -> 悬停 %d" % (base_top, hov_top))
        check("悬停图标包围盒变大（顶边升高 >= 3px）", base_top - hov_top >= 3,
              "base=%d hover=%d" % (base_top, hov_top))

        def left_edge(px, cx, span=30):
            for x in range(cx - span, cx + span):
                c = sample(px, w, x, dy + dh // 2)
                if sum(abs(c[i] - panel[i]) for i in range(3)) > 40:
                    return x
            return cx

        base_neigh = left_edge(px0, ncx)
        hov_neigh = left_edge(pxh, ncx)
        print("      左邻图标左边：基线 %d -> 悬停 %d" % (base_neigh, hov_neigh))
        check("邻位图标轻微让位（左边向外移 >= 2px）", base_neigh - hov_neigh >= 2,
              "base=%d hover=%d（外移 %d）" % (base_neigh, hov_neigh, base_neigh - hov_neigh))

        print("=== 3) 点击回弹（连续帧位移 + 弹簧 260ms）===")
        before_click = len(vm.log())
        mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.4)
        got = vm.wait_log("[DOCK64] bounce idx=%d" % HOVER_IDX, 6, since=before_click)
        check("点击产生回弹动画打点", got)
        time.sleep(1.6)
        seg = vm.log()[before_click:]
        frames = re.findall(r"\[DOCK64\] bounce idx=%d frame=(\d+) t=(\d+) dy=(\d+) dir=(\w+)" % HOVER_IDX, seg)
        done = re.search(r"\[DOCK64\] bounce idx=%d done frames=(\d+) motion=spring" % HOVER_IDX, seg)
        dys = [int(f[2]) for f in frames]
        dirs = [f[3] for f in frames]
        print("      回弹帧：%d 帧，dy 序列 %s，方向 %s" % (len(frames), dys[:10], dirs[:10]))
        check("回弹有连续帧（>= 4 帧）", len(frames) >= 4, "%d 帧" % len(frames))
        check("相邻帧位移在变（上下回弹，不是一步到位）",
              len(set(dys)) >= 3, "dy=%s" % dys[:10])
        check("弹簧曲线出现过冲（先向上、后向下回落）",
              ("up" in dirs) and ("down" in dirs or "rest" in dirs), "dirs=%s" % dirs[:10])
        check("回弹走完（done frames >= 6，约 260ms）",
              done is not None and int(done.group(1)) >= 6,
              done.group(0) if done else "（无 done 行）")

        print("=== 4) 最小化小横杠（40% 宽、强调色、不可点击）===")
        MINBAR_GEO = None
        # 先开计算器（开始菜单数字快捷键 4）
        mon.key("meta_l", wait=1.0)
        mon.key("4", wait=2.2)
        check("计算器已打开（菜单快捷键 4）", "[APP] calc opened" in vm.log())
        # 再点 Dock 上的计算器图标 -> 最小化
        calc = None
        for it in items_log:
            if it[1] == "1":       # APP_ID_CALC == 1
                calc = it
        check("Dock 里有计算器项", calc is not None)
        if calc:
            ccx, ccy = int(calc[7]), int(calc[8])
            # 光标此刻就停在计算器图标上（第 2/3 节闭环挪过来的）；Dock 区里 selbox 探测不到位置，
            # 所以这里不再 goto，直接点即可。
            before_min = len(vm.log())
            mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.6)
            minimized = vm.wait_log("[DOCK64] press idx=%s app=1 action=minimize" % calc[0], 6, since=before_min)
            check("点 Dock 图标把该应用最小化（action=minimize）", minimized,
                  vm.log()[before_min:before_min + 400].splitlines()[-1] if not minimized else "")
            mb = vm.wait_log("[DOCK64] minbar idx=", 6, since=before_min)
            check("最小化后出现小横杠打点", mb)
            m = re.search(r"\[DOCK64\] minbar idx=(\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) color=#([0-9A-Fa-f]{6}) clickable=0",
                          vm.log()[before_min:])
            check("小横杠打点含几何/颜色/clickable=0", m is not None, m.group(0) if m else "（无）")
            if m:
                bx, by, bw2 = int(m.group(2)), int(m.group(3)), int(m.group(4))
                col = tuple(int(m.group(6)[i:i + 2], 16) for i in (0, 2, 4))
                MINBAR_GEO = (bx, by, bw2)     # 供主题切换那节复用（紫白要按同一个横杠位置量像素）
                check("小横杠宽 = 图标宽 40%%（%d 的 40%% = %d）" % (icon, icon * 40 // 100),
                      bw2 == icon * 40 // 100, "w=%d" % bw2)
                check("小横杠高 3px 且位置在图标下方（Dock 内）",
                      int(m.group(5)) == 3 and dy < by < dy + dh, "y=%d" % by)
                time.sleep(0.8)
                shot_mb = os.path.join(tmp, "minbar.ppm")
                mon.shot(shot_mb)
                w3, h3, pxm = read_ppm(shot_mb)
                hit = 0
                for x in range(bx - 2, bx + bw2 + 2):
                    for y in range(by - 1, by + 4):
                        if near(sample(pxm, w3, x, y), col, 24):
                            hit += 1
                check("小横杠像素跟随强调色（横杠处强调色像素 >= 8）", hit >= 8,
                      "匹配像素=%d 期望色=%s" % (hit, col))
                # 纯视觉不可点击：内核自己对横杠中心做命中测试并打点（hit=-1 = 不是任何 Dock 项）
                ht = re.search(r"\[DOCK64\] minbar hit_test idx=(\d+) x=(\d+) y=(\d+) -> hit=none \(visual only, not clickable\)",
                               vm.log()[before_min:])
                check("横杠不可点击（内核命中测试 hit=none）",
                      ht is not None, ht.group(0) if ht else "（无）")

        print("=== 5) 主题切换（7 种，实时生效 + 像素差）===")
        shots = {}
        cur = re.search(r"\[THEME64\] init themes=(\d+) theme=(\d+)", vm.log())
        n_themes = int(cur.group(1)) if cur else -1
        theme_id = int(cur.group(2)) if cur else 0
        check("主题表 7 套（[THEME64] init themes=7：白/暗/蓝白/粉白/粉绿/粉紫/紫白）",
              n_themes == 7, "themes=%s" % n_themes)
        rc = re.search(r"\[THEME64\] rust cross-check count=(\d+) cpp=(\d+) count_ok=(\d)", vm.log())
        check("Rust 侧主题表交叉核对（数量一致 count_ok=1）",
              rc is not None and rc.group(1) == rc.group(2) == "7" and rc.group(3) == "1",
              rc.group(0) if rc else "（无）")
        shots[theme_id] = px0
        # 依次 Ctrl+Shift+T 切换 6 次（覆盖 7 个主题；单次切完断言打点与像素差）
        for step in range(6):
            want = (theme_id + 1) % 7
            before = len(vm.log())
            mon.key("ctrl-shift-t", wait=1.0)
            got = vm.wait_log("[THEME64] apply theme=%d" % want, 30)
            if not got:     # 按键偶发丢失：再按一次（QEMU sendkey 在多核/忙时可能丢）
                mon.key("ctrl-shift-t", wait=1.0)
                got = vm.wait_log("[THEME64] apply theme=%d" % want, 30)
            name, dark = THEME_EXPECT[want]
            ml = re.search(r"\[THEME64\] apply theme=%d name=%s dark=\d accent=#\w{6}" % (want, name), vm.log())
            check("切到主题 %d（%s）打点（含 name/dark/accent）" % (want, name), got and ml is not None,
                  ml.group(0) if ml else "（无）")
            repainted = wait_repaint64(vm, before)      # ★ 先等整屏按新主题重画完（TCG 下几秒）
            check("主题 %d 整屏按新主题重画完成（[GFX64] wall 行）" % want, repainted)
            time.sleep(0.6)                             # 让同一帧里的 Dock/时钟画完再截
            sp = os.path.join(tmp, "theme%d.ppm" % want)
            mon.shot(sp)
            wa, ha, pxa = read_ppm(sp)
            shots[want] = pxa
            d = diff_count(px0, pxa)
            check("主题 %d 的整屏像素与白色主题不同（变化 > 20 万像素）" % want,
                  d > 200000, "变化像素=%d" % d)
            # Dock 面板亮度（暗色主题必须变深灰）
            pc = sample(pxa, wa, gap_x, dy + dh // 2)
            lum = (pc[0] + pc[1] + pc[2]) / 3.0
            print("      主题 %d 面板色 %s 亮度 %.0f" % (want, pc, lum))
            if dark:
                check("暗色主题下 Dock 是深灰（亮度 < 110）", lum < 110, "%.0f" % lum)
            else:
                check("非暗色主题下 Dock 用浅色固定默认色（亮度 > 150）", lum > 150, "%.0f" % lum)
            if want == 6:
                # ---- 紫白渐变（参考图风格板）专属断言：强调色 #7C4DFF + Dock 紫调 + 小横杠像素 ----
                pm = re.search(r"\[THEME64\] apply theme=6 name=purplegrad dark=0 accent=#([0-9A-Fa-f]{6})",
                               vm.log())
                check("紫白渐变的强调色 = #7C4DFF（打点）",
                      pm is not None and pm.group(1).upper() == "7C4DFF",
                      pm.group(0) if pm else "（无）")
                check("紫白渐变：Dock 面板浅色且带紫调（B >= R > G、亮度 > 150）",
                      lum > 150 and pc[2] >= pc[0] and pc[0] > pc[1],
                      "面板 %s 亮度 %.0f" % (pc, lum))
                check("紫白渐变：Dock 面板色与白色主题不同（主题真的换了 Dock 颜色）",
                      sum(abs(pc[i] - panel[i]) for i in range(3)) >= 8,
                      "紫白 %s vs 白 %s" % (pc, panel))
                if MINBAR_GEO:
                    bx2, by2, bw3 = MINBAR_GEO
                    purple = blue = 0
                    for x in range(bx2 - 2, bx2 + bw3 + 2):
                        for y in range(by2 - 1, by2 + 4):
                            c = sample(pxa, wa, x, y)
                            if near(c, (0x7C, 0x4D, 0xFF), 24):
                                purple += 1
                            elif near(c, (0x00, 0x78, 0xD7), 12):
                                blue += 1
                    check("紫白渐变：Dock 小横杠像素 = 强调色 #7C4DFF（紫像素 >= 8）",
                          purple >= 8, "紫像素=%d（横杠 %d,%d %dx3）" % (purple, bx2, by2, bw3))
                    check("紫白渐变：小横杠不再是白色主题的蓝 #0078D7（蓝像素 <= 4）",
                          blue <= 4, "蓝像素=%d" % blue)
                else:
                    check("紫白渐变：Dock 小横杠位置可用（第 4 节拿到了 minbar 几何）", False, "（无几何）")
            theme_id = want
        ns = len(shots)
        check("7 种主题都有整屏像素证据", ns >= 7, "主题数=%d" % ns)
        # 主色（强调色）随主题变化：暗色/蓝白 的 accent 打点不同
        accents = re.findall(r"\[THEME64\] apply theme=(\d+) name=\S+ dark=\d accent=#(\w{6})", vm.log())
        _acc = {a[1].upper() for a in accents}
        check("7 种主题的强调色互不相同（含紫白 #7C4DFF）",
              len(_acc) >= 7 and "7C4DFF" in _acc, "accent 集合=%s" % sorted(_acc))
        # 切回白（把系统留在干净状态，避免影响复跑）
        if theme_id != 0:
            back_before = len(vm.log())
            mon.key("ctrl-shift-t", wait=1.0)
            vm.wait_log("[THEME64] apply theme=0", 25)
            wait_repaint64(vm, back_before)     # ★ 等切回白色也重画完（下面第 6 节直接量壁纸像素）
            time.sleep(0.6)

        print("=== 6) 壁纸 6 种适应模式（几何 + 定位标记像素）===")
        mk = re.search(r"\[GFX64\] wall markers r=(\d+),(\d+) g=(\d+),(\d+) b=(\d+),(\d+) y=(\d+),(\d+) size=(\d+)", vm.log())
        check("[GFX64] wall markers 打点（4 个定位标记）", mk is not None, mk.group(0) if mk else "（无）")
        marks = {}
        if mk:
            marks["r"] = (int(mk.group(1)), int(mk.group(2)))
            marks["g"] = (int(mk.group(3)), int(mk.group(4)))
            marks["b"] = (int(mk.group(5)), int(mk.group(6)))
            marks["y"] = (int(mk.group(7)), int(mk.group(8)))
            msize = int(mk.group(9))
        else:
            msize = 16
        for want_mode, want_name in ((0, "fill"), (1, "fit"), (2, "stretch"), (3, "tile"), (4, "center"), (5, "span")):
            before = len(vm.log())
            if want_mode != 0:
                mon.key("ctrl-shift-n", wait=1.0)
            elif "[GFX64] wall mode=0 name=fill" not in vm.log():
                mon.key("ctrl-shift-n", wait=1.0)
            wl = vm.wait_log("[GFX64] wall mode=%d name=%s" % (want_mode, want_name), 30)
            if not wl and want_mode != 0:   # 按键偶发丢失：再按一次（QEMU sendkey 在多核/忙时可能丢）
                mon.key("ctrl-shift-n", wait=1.0)
                wl = vm.wait_log("[GFX64] wall mode=%d name=%s" % (want_mode, want_name), 30)
            check("模式 %d（%s）生效并打了几何行" % (want_mode, want_name), wl)
            time.sleep(2.5)
            spawn = os.path.join(tmp, "wall%d.ppm" % want_mode)
            mon.shot(spawn, wait=3.0)
            ww, wh, pxw = read_ppm(spawn)
            gl = re.search(r"\[GFX64\] wall mode=%d name=%s src=(\d+)x(\d+) screen=(\d+)x(\d+) dst=(-?\d+),(-?\d+),(\d+)x(\d+) scale=(\d+)/(\d+) crop=l(\d+),t(\d+),r(\d+),b(\d+) tiles=(\d+)x(\d+)" % (want_mode, want_name),
                           vm.log())
            check("模式 %d 几何行可解析（含 dst/scale/crop/tiles）" % want_mode, gl is not None,
                  gl.group(0) if gl else "（无）")
            if gl is None:
                continue
            sw, sh = int(gl.group(1)), int(gl.group(2))
            sW, sH = int(gl.group(3)), int(gl.group(4))
            mdx, mdy, mdw, mdh = (int(gl.group(i)) for i in range(5, 9))
            tiles_x = int(gl.group(15))
            # 模式定义自检（几何必须在屏幕上成立）
            if want_mode in (0, 5):     # 填充/跨屏：铺满 + 等比 + 有裁剪
                check("模式 %d 铺满屏幕（dst 覆盖全屏）" % want_mode,
                      mdx <= 0 and mdy <= 0 and mdx + mdw >= sW and mdy + mdh >= sH,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
                ratio = (mdw / float(sw)) / (mdh / float(sh))
                check("模式 %d 保持等比（不拉伸变形）" % want_mode, abs(ratio - 1.0) < 0.02, "比例=%.3f" % ratio)
            elif want_mode == 1:        # 适应：整张可见
                check("适应模式：dst 不超出屏幕", mdx >= 0 and mdy >= 0 and mdx + mdw <= sW and mdy + mdh <= sH,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            elif want_mode == 2:        # 拉伸：铺满（允许变形）
                check("拉伸模式：dst == 全屏", (mdx, mdy, mdw, mdh) == (0, 0, sW, sH),
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            elif want_mode == 3:        # 平铺：1:1 且不止一块
                check("平铺模式：1:1 尺寸（dst == 源尺寸）", mdw == sw and mdh == sh,
                      "dst=%dx%d src=%dx%d" % (mdw, mdh, sw, sh))
            elif want_mode == 4:        # 居中：1:1 居中
                check("居中模式：1:1 且屏幕居中",
                      mdw == sw and mdh == sh and abs(mdx - (sW - sw) // 2) <= 1 and abs(mdy - (sH - sh) // 2) <= 1,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            # 定位标记：源坐标 -> 屏幕坐标
            def mark_screen(sx, sy):
                if want_mode == 3:
                    return (mdx + sx, mdy + sy)
                return (mdx + int(round(sx * mdw / float(sw))), mdy + int(round(sy * mdh / float(sh))))

            for key, (sx, sy) in marks.items():
                ex, ey = mark_screen(sx, sy)
                cx2, cy2 = ex + max(1, int(msize * mdw / float(sw) / 2)), ey + max(1, int(msize * mdh / float(sh) / 2))
                if not (2 <= cx2 < sW - 2 and 2 <= cy2 < sH - 2):
                    continue            # 该标记被裁掉（预期：填充模式下四角标记都在屏内，这里只跳过越界）
                # 被 Dock / 时钟片挡住的标记跳过（Dock 是顶层，标记被盖住看不到）
                if dy - 2 <= cy2 <= dy + dh + 2 and (dx - 2 <= cx2 <= dx + dw + 2 or
                                                     dclk[0] - 2 <= cx2 <= dclk[0] + dclk[1] + 2):
                    continue
                c = sample(pxw, ww, cx2, cy2)
                if abs(c[0] - MARK_COLORS[key][0]) > 30 or abs(c[1] - MARK_COLORS[key][1]) > 30 or \
                        abs(c[2] - MARK_COLORS[key][2]) > 30:
                    # 找一找 ±6px 邻域里有没有（缩放取整误差）
                    found = False
                    for oy in range(-6, 7):
                        for ox in range(-6, 7):
                            c2 = sample(pxw, ww, min(sW - 1, max(0, cx2 + ox)), min(sH - 1, max(0, cy2 + oy)))
                            if abs(c2[0] - MARK_COLORS[key][0]) <= 30 and abs(c2[1] - MARK_COLORS[key][1]) <= 30 and \
                                    abs(c2[2] - MARK_COLORS[key][2]) <= 30:
                                found = True
                                break
                        if found:
                            break
                else:
                    found = True
                check("模式 %d：%s 标记在预期位置（源 %d,%d -> 屏 %d,%d）" % (want_mode, key, sx, sy, cx2, cy2),
                      found, "实测色 %s 期望 %s" % (c, MARK_COLORS[key]))
            # 模式专属结构证据
            if want_mode == 1:      # 适应：左右留白（无图区域接近纯色）
                strip_var = 0
                vals = [sample(pxw, ww, x, 400)[1] for x in range(4, mdx - 6, 4)]
                if len(vals) > 4:
                    mean = sum(vals) / len(vals)
                    strip_var = sum((v - mean) ** 2 for v in vals) / len(vals)
                in_vals = [sample(pxw, ww, x, 400)[1] for x in range(mdx + 8, mdx + 120, 4)]
                in_mean = sum(in_vals) / len(in_vals)
                in_var = sum((v - in_mean) ** 2 for v in in_vals) / len(in_vals)
                check("适应模式：留白区是纯色底（方差远低于图内）",
                      strip_var * 3 < max(1.0, in_var), "留白方差 %.2f 图内方差 %.2f" % (strip_var, in_var))
            if want_mode == 3:      # 平铺：第二块瓷砖的接缝（x = 源宽 处复制源 x=0 列）
                ok_seam = True
                for y in range(60, 700, 37):
                    if ww > 1200 and sw == 1200:
                        c0 = sample(pxw, ww, y % 2, y)
                        c1 = sample(pxw, ww, 1200 + (y % 2), y)
                        if sum(abs(c0[i] - c1[i]) for i in range(3)) > 6:
                            ok_seam = False
                            break
                check("平铺模式：x=1200 列与 x=0 列相同（真的重复铺）", ok_seam and tiles_x >= 1)
            if want_mode == 2:      # 拉伸：红标记被拉扁（宽高比 != 1）
                def bbox_of(tcol, tol=40):
                    xs, ys = [], []
                    for yy in range(0, sH, 2):
                        for xx in range(0, sW, 2):
                            cc = sample(pxw, ww, xx, yy)
                            if abs(cc[0] - tcol[0]) <= tol and abs(cc[1] - tcol[1]) <= tol and abs(cc[2] - tcol[2]) <= tol:
                                xs.append(xx)
                                ys.append(yy)
                    if not xs:
                        return None
                    return (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
                bb = bbox_of(MARK_COLORS["r"])
                check("拉伸模式：标记宽高比 != 1（图被拉变形）", bb is not None and abs(bb[0] / float(bb[1]) - 1.0) > 0.08,
                      "bbox=%s" % (bb,))

        print("=== 7) 减少动画开关 ===")
        before = len(vm.log())
        mon.key("ctrl-shift-r", wait=1.0)
        got = vm.wait_log("[THEME64] motion reduce=1", 8, since=before)
        check("Ctrl+Shift+R 打开减少动画（[THEME64] motion reduce=1）", got,
              (re.search(r"\[THEME64\] motion reduce=1[^\r\n]*", vm.log()[before:]) or [""])[0] if got else "")
        mrm = re.search(r"\[THEME64\] motion reduce=1 fast=\d+ normal=\d+ large=\d+ dock=(\d+)ms", vm.log())
        check("减少动画时动效时长退化为 0ms", mrm is not None and mrm.group(1) == "0",
              mrm.group(0) if mrm else "（无）")
        # 点一个 Dock 图标 -> 回弹 1 帧到位
        goto(vm, mon, tcx, tcy)
        before_rm = len(vm.log())
        mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.6)
        rm = vm.wait_log("[DOCK64] bounce idx=%d frames=1 motion=reduced" % HOVER_IDX, 6)
        check("减少动画：点击回弹 1 帧到位（bounce frames=1 motion=reduced）", rm)
        before = len(vm.log())
        mon.key("ctrl-shift-r", wait=1.0)
        back = vm.wait_log("[THEME64] motion reduce=0", 8)
        check("再按一次恢复动画（reduce=0）", back)

        # ==================== 8) Dock 开始按钮 = 真图 logo/kaisi.png（带 VimtuFS2 系统卷的盘）====================
        print("=== 8) Dock 开始按钮用真图 logo/kaisi.png（VimtuFS2 系统卷夹具盘）===")
        vm.close()          # 主 VM 收工：下面两次启动都在夹具盘上（省 CPU，别影响夹具盘的启动时序）
        with open(PNG_LOGO, "rb") as f:
            png_src = f.read()
        check("宿主侧 logo/kaisi.png 可读（真文件）", len(png_src) > 0, "%d 字节" % len(png_src))
        render, png_dim = png_render46(png_src)
        check("宿主 Pillow 解 logo/kaisi.png 并缩到 46x46（内核 dock_rgba_from_img64 同一映射）",
              render is not None and len(render) == 46 * 46,
              "PNG 尺寸=%s 不透明像素=%s" % (png_dim,
                                            sum(1 for c in render if c[3] >= 252) if render else "?"))
        fixture = os.path.join(tmp, "small_system.img")
        finfo = make_vfs_fixture(fixture, args.img)
        check("造夹具盘（system.img 字节 + MBR + @8009 的空 v3 卷）", finfo is not None,
              fixture if finfo else "（fs_tree_test 不可用 / 镜像不合法）")
        if finfo and render is not None:
            # ---- 8a) 第一遍：空卷 -> 启动期安装 -> 从盘上读真图 -> 像素比对 ----
            vm1 = Vm(qemu, fixture, args.port + 7, "vimtu-modern-vfs1", tmp)
            try:
                mon1 = vm1.wait_monitor()
                check("夹具盘进桌面（[GUI64] ready）", vm1.wait_ready(120))
                L1 = vm1.log()
                inst = re.search(r"\[IMG64\] install path=/logo/kaisi\.png bytes=(\d+) written=(\d+) ok=(\d) src=(\S+)", L1)
                check("启动期把内嵌真 PNG 幂等装进 VimtuFS2 系统卷（/logo/kaisi.png ok=1）",
                      inst is not None and inst.group(3) == "1" and
                      inst.group(1) == inst.group(2) == str(len(png_src)),
                      inst.group(0) if inst else "（无）")
                inst2 = re.search(r"\[IMG64\] install path=/kaisi\.png bytes=(\d+) written=(\d+) ok=(\d)", L1)
                check("兜底路径 /kaisi.png 也装了（同一份字节）",
                      inst2 is not None and inst2.group(3) == "1" and inst2.group(1) == str(len(png_src)),
                      inst2.group(0) if inst2 else "（无）")
                load = re.search(r"\[IMG64\] load path=/logo/kaisi\.png ok=1 bytes=(\d+) fmt=png "
                                 r"(\d+)x(\d+) \(from VimtuFS2 system volume\)", L1)
                check("从 VimtuFS2 系统卷读真图（[IMG64] load path=/logo/kaisi.png ok=1）",
                      load is not None and load.group(1) == str(len(png_src)) and
                      (int(load.group(2)), int(load.group(3))) == png_dim,
                      load.group(0) if load else "（无）")
                # ★ 启动自检的"真文件"用例（1 个大动态 Huffman 块 + 收尾空固定块 + 100 KB 输出）：
                #   内核解出的像素指纹必须等于宿主 PIL 解同一份文件的指纹（老 inflate 在这里 rc=18）
                sr = re.search(r"\[IMG64\] selftest real ok=(\d+) bytes=(\d+) (\d+)x(\d+) px=(\d+) "
                               r"fnv=([0-9A-Fa-f]{16}) rc=(\d+)", L1)
                hfnv = png_full_fnva(png_src)
                check("启动自检解真文件 logo/kaisi.png（[IMG64] selftest real ok=1 rc=0）",
                      sr is not None and sr.group(1) == "1" and sr.group(7) == "0",
                      sr.group(0) if sr else (re.search(r"\[IMG64\] selftest real[^\r\n]*", L1) or ["（无）"])[0])
                if sr is not None and hfnv is not None:
                    check("自检像素指纹 = 宿主 PIL 解码同一文件（FNV-1a 32，逐字节）",
                          int(sr.group(2)) == len(png_src) and int(sr.group(6), 16) == hfnv,
                          "内核 bytes=%s fnv=%s / 宿主 bytes=%d fnv=%08X"
                          % (sr.group(2), sr.group(6), len(png_src), hfnv))
                check("没有走 skip（有卷时不许 [IMG64] load skip path=/logo/kaisi.png）",
                      "[IMG64] load skip path=/logo/kaisi.png" not in L1)
                sx1 = re.search(r"\[DOCK64\] start icon src=(\S+) size=(\d+) ok=(\d)", L1)
                check("Dock 开始按钮用的是盘上真图（src=vfs:/logo/kaisi.png 或 /logo/kaisi.png）",
                      sx1 is not None and dock_src_is_vfs(sx1.group(1)) and sx1.group(3) == "1",
                      sx1.group(0) if sx1 else "（无）")
                check("夹具盘第一遍没有 PANIC/selftest FAIL/OOM",
                      "PANIC" not in L1 and "selftest FAIL" not in L1 and "OOM:" not in L1)
                it0 = re.search(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+)", L1)
                if it0 is None:
                    check("开始按钮几何（[DOCK64] item idx=0）", False, "（无）")
                else:
                    x0, y0, w0, h0 = (int(it0.group(i)) for i in range(1, 5))
                    time.sleep(1.5)
                    shotv = os.path.join(tmp, "vfs_png.ppm")
                    mon1.shot(shotv)
                    wv, hv, pxv = read_ppm(shotv)
                    check("夹具盘截图分辨率 1280x800", (wv, hv) == (1280, 800), "%dx%d" % (wv, hv))
                    panv = sample(pxv, wv, x0 + w0 + 5, y0 + h0 // 2)      # 图标 0/1 之间的纯面板
                    # (a) 用户口径：中心区主色（PIL 解真图 -> 46x46 -> 中心 24x24，合成到面板色上）
                    ec = [0, 0, 0]
                    mc = [0, 0, 0]
                    n = 0
                    for j in range(11, 35):
                        for i in range(11, 35):
                            r_, g_, b_, a_ = render[j * 46 + i]
                            for k, v in enumerate((r_, g_, b_)):
                                ec[k] += (v * a_ + panv[k] * (255 - a_)) // 255
                            c = sample(pxv, wv, x0 + i, y0 + j)
                            for k in range(3):
                                mc[k] += c[k]
                            n += 1
                    exp = tuple(v // n for v in ec)
                    got = tuple(v // n for v in mc)
                    check("中心区主色：宿主 PIL 解真图 == QEMU 截图（容差 12/通道）",
                          all(abs(exp[k] - got[k]) <= 12 for k in range(3)),
                          "宿主 %s 截图 %s（面板 %s）" % (exp, got, panv))
                    # (b) 像素级：alpha>=252 的像素内核是**原样拷贝**（blit_rgba_scaled: a>=252 直接写 RGB）
                    okpx = tot = maxd = 0
                    for j in range(46):
                        for i in range(46):
                            r_, g_, b_, a_ = render[j * 46 + i]
                            if a_ < 252:
                                continue
                            tot += 1
                            c = sample(pxv, wv, x0 + i, y0 + j)
                            dd = max(abs(c[k] - (r_, g_, b_)[k]) for k in range(3))
                            maxd = max(maxd, dd)
                            if dd <= 6:
                                okpx += 1
                    check("逐像素：真图的不透明像素与截图一致（>=90%，最大偏差 <= 6）",
                          tot >= 400 and okpx * 100 >= tot * 90 and maxd <= 6,
                          "一致 %d/%d，最大偏差 %d" % (okpx, tot, maxd))
                # ---- 宿主侧直读夹具盘：两处真图都与 logo/kaisi.png 逐字节相同 ----
                with open(fixture, "rb") as f:
                    disk = bytearray(f.read())
                vol_off = finfo["main_lba"] * SECTOR
                inos = v3_find_inodes(disk, finfo["main_lba"], b"kaisi.png")
                check("夹具盘 v3 卷里有两处 kaisi.png（/logo/kaisi.png + /kaisi.png）",
                      len(inos) >= 2, "inode=%s" % [(i, p) for _, i, p in inos])
                same = size_ok = 0
                for o, _i, _p in inos:
                    sz, data = v3_read_file(disk, vol_off, o)
                    if sz == len(png_src):
                        size_ok += 1
                    if data == png_src:
                        same += 1
                check("盘上两处文件大小都 = %d B（= 真文件大小）" % len(png_src), size_ok >= 2,
                      "命中 %d 处" % size_ok)
                check("盘上两处文件都与 logo/kaisi.png 逐字节相同（VFS 真图，不是兜底副本）", same >= 2,
                      "逐字节相同 %d 处" % same)
                # ---- 8b) 第二遍：把盘上 /logo/kaisi.png 的第一块改坏（大小不变）-> 冷启动 ----
                logo_ino = root_ino = None
                for o, _i, par in inos:
                    if par != 0:
                        logo_ino = o
                    else:
                        root_ino = o
                check("能按 parent 区分 /logo/kaisi.png（parent != 0）与根目录那份",
                      logo_ino is not None and root_ino is not None,
                      "logo=%s root=%s" % (logo_ino, root_ino))
                if logo_ino is not None:
                    blk = v3_corrupt_file(disk, vol_off, logo_ino)
                    with open(fixture, "wb") as f:
                        f.write(disk)
                    print("      已把盘上 /logo/kaisi.png 的第 1 块（fs 块 %d）前 64 B 改坏（大小不变）" % blk)
                    vm2 = Vm(qemu, fixture, args.port + 8, "vimtu-modern-vfs2", tmp)
                    try:
                        vm2.wait_monitor()
                        check("冷启动第二遍进桌面（同一夹具盘）", vm2.wait_ready(120))
                        L2 = vm2.log()
                        check("幂等：第二遍 install 打 reason=exists（盘上已有同大小文件）",
                              "[IMG64] install skip path=/logo/kaisi.png reason=exists bytes=%d" % len(png_src) in L2,
                              (re.search(r"\[IMG64\] install skip path=/logo/kaisi\.png[^\r\n]*", L2) or ["（无）"])[0])
                        check("盘上那份被改坏 -> 解码失败（[IMG64] load path=/logo/kaisi.png ok=0）",
                              re.search(r"\[IMG64\] load path=/logo/kaisi\.png ok=0 bytes=\d+ err=", L2) is not None,
                              (re.search(r"\[IMG64\] load path=/logo/kaisi\.png[^\r\n]*", L2) or ["（无）"])[0])
                        check("于是落到 VimtuFS2 的第二条候选 /kaisi.png（load ok=1 + src=vfs:/kaisi.png）",
                              re.search(r"\[IMG64\] load path=/kaisi\.png ok=1 bytes=\d+ fmt=png", L2) is not None and
                              re.search(r"\[DOCK64\] start icon src=(?:vfs:)?/kaisi\.png size=\d+ ok=1", L2) is not None,
                              (re.search(r"\[DOCK64\] start icon src=\S+[^\r\n]*", L2) or ["（无）"])[0])
                        check("第二遍没有 PANIC/selftest FAIL/OOM",
                              "PANIC" not in L2 and "selftest FAIL" not in L2 and "OOM:" not in L2)
                    finally:
                        vm2.close()
            finally:
                vm1.close()

        print("=== 9) 不能出现的日志 ===")
        log = slog = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:", "kfree: bad"):
            check("不应出现 %s" % bad, bad not in log)
        if args.keep:
            print("截图/串口：%s" % tmp)
    finally:
        vm.close()

    n_pass = sum(1 for _, c in checks if c)
    print("断言：%d/%d PASS" % (n_pass, len(checks)))
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
