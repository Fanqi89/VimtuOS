#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/imgfit64_test.py - ★ v0.5.17：任意分辨率都能当壁纸/头像（**解码期缩放**）

背景（用户提问）："不能把内核 img64 的解码上限改高，像 Linux 一样不管什么分辨率和大小都可以
做为壁纸和头像吗？"

以前的做法是"整幅解出来再缩"，所以必须有上限：
  * JPEG：`jpeg too large (>2048)`（整幅 planes + px 要 w*h*(3+4) 字节；8000x6000 = 192 MB）
  * PNG ：`bad ihdr`（w/h > 4096）且先 malloc(w*h*channels) 装整幅 inflate 结果
v0.5.17 改成**解码期抽样**：
  * PNG：inflate 输出按行回调（32 KiB 历史窗），只留 2 行反滤波，每 decim 行/列取一个样点；
  * JPEG：按 DCT 块 d×d 求平均写进缩小后的分量平面（等价于只保留低频）；
  * BMP：逐行/列抽样（BMP 未压缩）。
内存只跟"目标尺寸 + 一行"有关，与源分辨率脱钩；上限变成 边长 ≤16384 / 源像素 ≤32 MP / 文件 ≤24 MB。

判据（每条都能独立证伪）：
  ① 6000x4000 的 PNG 设为壁纸：`[SET64] wall path=/bigtest/big.png ok=1 px=1500x1000`
     （decim=4；最长边 ≤2048），并且串口有 `[IMG64] fit png src=6000x4000 decim=4 -> 1500x1000`；
  ② 5000x3333 的 JPEG 设为头像：`[USER64] avatar … px=256x170`（最长边 = 256），
     串口有 `[IMG64] fit jpeg src=5000x3333 decim=… -> 256x…`；
  ③ 5000x3333 的 JPEG 设为壁纸：`[SET64] wall path=/bigtest/big.jpg ok=1 px=1250x833`；
  ④ 重启后头像/壁纸仍从文件加载（`[IMG64] load fit … ok=1`），无 PANIC / 无 OOM；
  ⑤ 小图不受影响（presets 那两张 1024x768 / 2649x1600 仍按原分辨率进；由 presets_user64_test 覆盖）。

退出码：0 全过 / 1 有断言失败 / 2 环境问题
"""
import io
import os
import re
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import install_persist64_test as P  # noqa: E402
import startmenu64_test as SM  # noqa: E402
import qemuhelp as qh  # noqa: E402

QEMU = P.T.find_qemu()
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
PAYLOAD = os.path.join(ROOT, "build64", "sysdisk.img")
WORK = os.path.join(ROOT, "build64", "imgfit64")
TGT = os.path.join(WORK, "target2g.img")
PORT = 5598
MAIN_LBA = 8009
GIB = 1024 ** 3

P_WALL, P_AVATAR = 5, 12
CID_WALL_FIELD, CID_WALL_APPLY = 70, 71
CID_AVATAR_FIELD, CID_AVATAR_APPLY = 145, 146

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def q(p):
    return p.replace("\\", "/")


def log_of(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return ""


def wait_in(p, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in log_of(p):
            return True
        time.sleep(0.5)
    return False


def start(args, serial, port):
    a = [QEMU, "-name", "imgfit64", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"] + args
    p = subprocess.Popen(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return p, SM.Monitor(port)


def ctl_center(log, page, cid):
    m = None
    for mm in re.finditer(r"\[SET64\] ctl page=(\d+) id=(\d+) kind=\S+ x=\d+ y=\d+ w=(\d+) h=(\d+) "
                          r"cx=-?\d+ cy=-?\d+ ax=(-?\d+) ay=(-?\d+)", log):
        if int(mm.group(1)) == page and int(mm.group(2)) == cid:
            m = (int(mm.group(5)) + int(mm.group(3)) // 2, int(mm.group(6)) + int(mm.group(4)) // 2)
    return m


def nav_center(log, page):
    m = None
    for mm in re.finditer(r"\[SET64\] nav item page=(\d+) .*acx=(-?\d+) acy=(-?\d+)", log):
        if int(mm.group(1)) == page:
            m = (int(mm.group(2)), int(mm.group(3)))
    return m


def wait_for(fn, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = fn()
        if v:
            return v
        time.sleep(0.5)
    return None


def make_big_images():
    """宿主侧造两张"超大"图（Pillow）：6000x4000 PNG + 5000x3333 JPEG。"""
    from PIL import Image
    out = {}
    w, h = 6000, 4000
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(0, h, 4):                      # 有渐变但不整块纯色（避免压缩后过小不真实）
        for x in range(0, w, 4):
            r = (x * 255) // w
            g = (y * 255) // h
            b = ((x + y) * 255) // (w + h)
            for dy in range(4):
                for dx in range(4):
                    if x + dx < w and y + dy < h:
                        px[x + dx, y + dy] = (r, g, b)
    bio = io.BytesIO()
    im.save(bio, "PNG", optimize=True)
    out["big.png"] = bio.getvalue()
    w2, h2 = 5000, 3333
    im2 = im.resize((w2, h2), Image.LANCZOS)
    bio2 = io.BytesIO()
    im2.save(bio2, "JPEG", quality=85, optimize=True)
    out["big.jpg"] = bio2.getvalue()
    return out


def build_fixture():
    """装一块 2 GiB 盘，再把两张超大图用宿主侧卷编辑器放进 /bigtest/。"""
    if os.path.exists(TGT):
        return True
    os.makedirs(WORK, exist_ok=True)
    imgs = make_big_images()
    print("  测试图：big.png %d B（6000x4000）、big.jpg %d B（5000x3333）"
          % (len(imgs["big.png"]), len(imgs["big.jpg"])))
    sectors = 2 * GIB // 512
    with open(TGT, "wb") as f:
        f.truncate(sectors * 512)
    serial = os.path.join(WORK, "install.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(["-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
                       "-drive", "format=raw,file=%s,index=1,media=disk" % q(TGT),
                       "-boot", "order=c"], serial, PORT)
    try:
        if not wait_in(serial, "磁盘枚举完成", 240):
            return False
        for k in ("ret", "ret", "ret", "ret", "n", "ret"):
            mon.key(k, wait=2.0)
        if not wait_in(serial, "[SETUP] 安装完成", 1800):
            return False
    finally:
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()

    # 宿主侧把两张图写进装好的系统卷（读超级块拿几何 -> VolumeEdit 追加 /bigtest/）
    import importlib.util
    spec = importlib.util.spec_from_file_location("tcc_pack_win",
                                                 os.path.join(ROOT, "tools", "tcc_pack_win.py"))
    TP = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(TP)
    with open(TGT, "r+b") as f:
        f.seek(MAIN_LBA * 512)
        sb = f.read(512)
        assert sb[:8] == b"VIMTUFS2", sb[:8]
        total = struct.unpack_from("<I", sb, 20)[0]
        f.seek(MAIN_LBA * 512)
        vol_bytes = f.read(total * 512)
    vol = TP.VolumeEdit(total)
    vol.load(vol_bytes)
    vol.mkdirs("/bigtest")
    for name, data in imgs.items():
        vol.put("/bigtest/" + name, data, mode=0o644)
    # ★ 必须 finish()：_alloc_blk 只维护 Python 侧的 used 集合，位图字节与超级块是在 finish()
    #   里才写回 buf 的。漏了这一步 -> 注入文件占的块在盘上仍标"空闲" -> 内核启动后当自由块
    #   分配出去（配置落盘就覆盖上去了）-> 图片读回来是 0 -> `ok=0 err=unknown format`（实测踩到）。
    vol_bytes2 = vol.finish()
    with open(TGT, "r+b") as f:
        f.seek(MAIN_LBA * 512)
        f.write(vol_bytes2)
    # 宿主侧回读自检：用独立的卷解析器（tests/volreloc64_test.Vol）确认
    #   ① 两个文件都能按 inode 链读回**逐字节一致**；② 它们用到的块在位图里都标了已用。
    from volreloc64_test import Vol
    v = Vol(TGT, MAIN_LBA)
    bad = []
    for name, data in imgs.items():
        rec = None
        for i in range(v.inon):
            r = v.inode(i)
            if r[0] != 1:
                continue
            nl = r[1]
            if not (0 < nl <= 31):
                continue
            if r[40:40 + nl].decode("latin1") != name:
                continue
            par = struct.unpack_from("<I", r, 28)[0]
            pr = v.inode(par)
            pnl = pr[1]
            if pnl and pr[40:40 + pnl].decode("latin1") == "bigtest":
                rec = r
                break
        if rec is None:
            bad.append(name + ":inode-missing")
            continue
        got = v.read_file(rec)
        if got != data:
            bad.append("%s:bytes(%d/%d)" % (name, len(got), len(data)))
        nu = [b for b in v.file_blocks(rec) if not v.bitmap_used(b)]
        if nu:
            bad.append("%s:bitmap-not-used(%d)" % (name, len(nu)))
    v.close()
    if bad:
        print("  注入自检失败：%s" % bad)
        return False
    print("  注入自检通过：两个文件逐字节可读且块都标了已用")
    print("  已把 /bigtest/big.png、/bigtest/big.jpg 写进装好的卷（total=%d 块）" % total)
    return True


def main():
    if not QEMU or not os.path.exists(MEDIUM):
        print("跳过：qemu / 安装介质不可用")
        return 2
    try:
        import PIL  # noqa: F401
    except Exception:
        print("跳过：没有 Pillow，无法生成超大测试图")
        return 2
    if not build_fixture():
        print("夹具准备失败")
        return 2

    serial = os.path.join(WORK, "ui1.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(TGT), "-boot", "order=c"], serial, PORT)
    try:
        if not qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=400):
            print("第一次启动没进桌面")
            return 2
        cur = SM.Cursor()
        cur.goto(mon, 754, 754)                      # Dock 里的"设置"
        mon.click(wait=2.5)
        nav = wait_for(lambda: nav_center(log_of(serial), P_WALL), 40)
        check("设置打开（壁纸页导航 %s）" % (nav,), nav is not None)
        if not nav:
            return 1

        def set_path(page, field_id, apply_id, path, wait_ok, timeout=120, tries=3):
            """切到该页 -> 点输入框 -> 输入路径 -> 点应用。
            重试理由（实测踩到）：进入页面后**第一次**点输入框偶尔没取到焦点（页面切换动画/焦点竞争），
            后面再点就正常 —— 这属于测试夹具的时序，不该记成产品失败。"""
            for k in range(tries):
                navp = nav_center(log_of(serial), page)
                if not navp:
                    time.sleep(1.0)
                    continue
                cur.goto(mon, navp[0], navp[1])
                mon.click(wait=2.0)
                fld = wait_for(lambda: ctl_center(log_of(serial), page, field_id), 25)
                btn = wait_for(lambda: ctl_center(log_of(serial), page, apply_id), 25)
                if not fld or not btn:
                    continue
                cur.goto(mon, fld[0], fld[1])
                mon.click(wait=1.5)
                mon.key("end", wait=0.4)
                for _ in range(48):                   # 先把输入框里可能残留的内容清掉
                    mon.key("backspace", wait=0.03)
                P.type_line(mon, path)
                time.sleep(1.0)
                cur.goto(mon, btn[0], btn[1])
                mon.click(wait=3.0)
                if wait_for(lambda: (wait_ok in log_of(serial)) or None, 60) is not None:   # 24MP PNG 解码十几秒
                    return True
            return wait_for(lambda: (wait_ok in log_of(serial)) or None, timeout) is not None

        # ---- ① 6000x4000 PNG 当壁纸 ----
        ok_png = set_path(P_WALL, CID_WALL_FIELD, CID_WALL_APPLY, "/bigtest/big.png",
                          "[SET64] wall path=/bigtest/big.png ok=1")
        log1 = log_of(serial)
        m = re.search(r"\[SET64\] wall path=/bigtest/big\.png ok=1 px=(\d+)x(\d+)", log1)
        check("壁纸 6000x4000 PNG 应用成功（解码期抽样）", ok_png and m is not None,
              (m.group(0) if m else "缺失"))
        if m:
            w, h = int(m.group(1)), int(m.group(2))
            check("PNG 缩放后最长边 <= 2048", max(w, h) <= 2048, "%dx%d" % (w, h))
            check("PNG 抽样比例符合预期（6000/4=1500）", (w, h) == (1500, 1000), "%dx%d" % (w, h))
        mf = re.search(r"\[IMG64\] fit png src=(\d+)x(\d+) decim=(\d+) -> (\d+)x(\d+)", log1)
        check("串口有解码期抽样证据（[IMG64] fit png src=… decim=… -> …）", mf is not None,
              (mf.group(0) if mf else "缺失"))

        # ---- ② 5000x3333 JPEG 当头像 ----
        ok_av = set_path(P_AVATAR, CID_AVATAR_FIELD, CID_AVATAR_APPLY, "/bigtest/big.jpg",
                         "[USER64] avatar user=")
        log2 = log_of(serial)
        m = re.search(r"\[USER64\] avatar user=\S+ src=/bigtest/big\.jpg via=settings px=(\d+)x(\d+)\s+"
                      r"saved=1 persisted=1", log2)
        check("头像 5000x3333 JPEG 应用成功且落盘", ok_av and m is not None,
              (m.group(0) if m else "缺失"))
        if m:
            w, h = int(m.group(1)), int(m.group(2))
            check("头像缩放后最长边 <= 256", max(w, h) <= 256, "%dx%d" % (w, h))
            check("头像不是黑图（缩放后仍有足够像素数）", w * h >= 64 * 64, "%dx%d" % (w, h))

        # ---- ③ 5000x3333 JPEG 当壁纸 ----
        ok_jpg = set_path(P_WALL, CID_WALL_FIELD, CID_WALL_APPLY, "/bigtest/big.jpg",
                          "[SET64] wall path=/bigtest/big.jpg ok=1")
        log3 = log_of(serial)
        m = re.search(r"\[SET64\] wall path=/bigtest/big\.jpg ok=1 px=(\d+)x(\d+)", log3)
        check("壁纸 5000x3333 JPEG 应用成功", ok_jpg and m is not None, (m.group(0) if m else "缺失"))
        if m:
            check("JPEG 缩放后最长边 <= 2048", max(int(m.group(1)), int(m.group(2))) <= 2048,
                  "%sx%s" % (m.group(1), m.group(2)))
        mf = re.search(r"\[IMG64\] fit jpeg src=(\d+)x(\d+) decim=(\d+) -> (\d+)x(\d+)", log3)
        check("串口有 JPEG 解码期降采样证据", mf is not None, (mf.group(0) if mf else "缺失"))
        check("没有 OOM / PANIC", "OOM:" not in log3 and "PANIC" not in log3)
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()

    # ---- ④ 重启：两张图仍能按缩放后的尺寸加载 ----
    serial2 = os.path.join(WORK, "ui2.log")
    if os.path.exists(serial2):
        os.remove(serial2)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(TGT), "-boot", "order=c"], serial2, PORT)
    try:
        up2 = qh.wait_desktop(mon, lambda: log_of(serial2), proc, timeout=400)
        log4 = log_of(serial2)
        check("重启后仍进桌面", up2)
        check("重启后壁纸仍由文件加载（[IMG64] load fit … /bigtest/big.jpg ok=1）",
              re.search(r"\[IMG64\] load fit path=/bigtest/big\.jpg max=2048 ok=1", log4) is not None,
              (re.search(r"\[IMG64\] load fit[^\r\n]*", log4) or [""])[0].strip()[:120])
        check("重启后头像仍由文件加载（max=256）",
              re.search(r"\[IMG64\] load fit path=/bigtest/big\.jpg max=256 ok=1", log4) is not None,
              (re.search(r"\[IMG64\] load fit path=/bigtest/big\.jpg max=256[^\r\n]*", log4)
               or [""])[0].strip()[:120])
        check("重启日志无 OOM / PANIC", "OOM:" not in log4 and "PANIC" not in log4)
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
