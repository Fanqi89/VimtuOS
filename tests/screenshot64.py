#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/screenshot64.py - 抓一张 64 位桌面的真实截图（PNG），供人工查看/留档

做的事：
  1) 引导 build64/system.img（= 装好的系统，直接进桌面）
  2) 等 [GUI64] ready
  3) 用 Win 键 + 数字快捷键打开指定应用（默认：扫雷、计算器、任务管理器）
  4) QEMU monitor screendump 抓帧 -> Pillow 转 PNG -> 存到 docs/screenshots/

用法：
    python tests/screenshot64.py                       # 默认开扫雷/计算器/任务管理器
    python tests/screenshot64.py --apps 5,4            # 只开扫雷(5)和计算器(4)
    python tests/screenshot64.py --out X.png
    python tests/screenshot64.py --no-apps             # 只抓纯净桌面
    python tests/screenshot64.py --page thispc|drive|details
        # ★ 本轮新增：抓"文件资源管理器"三种页面（此电脑 / 盘内图标视图 / 详细信息四列）。
        #   这三张图必须有一块**带 VimtuFS2 卷的已装系统盘**才好看，所以 --page 模式下默认用
        #   tests/fs_tree_test.py 的 Python 侧 v3 卷夹具造一块 16MB 小系统盘（--img 显式给出时除外）；
        #   导航用键盘（Win+2 开窗 -> 下 -> 回车进 C: -> Tab 切视图），不依赖鼠标注入。

菜单序号（与 kernel/gui64.cpp 一致）：1终端 2我的电脑 3系统监视器 4计算器 5扫雷 6设置 7任务管理器 8关于
"""
import argparse
import os
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

# 数字键 -> 菜单序号（'1'=第0项 ... '0'=第9项）
DIGIT_OF_INDEX = {0: "1", 1: "2", 2: "3", 3: "4", 4: "5", 5: "6", 6: "7", 7: "8", 8: "9", 9: "0"}


def find_qemu():
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def mon_send(port, cmd, wait=0.6):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=8)
    except OSError:
        return
    try:
        s.sendall(cmd.encode() + b"\n")
        time.sleep(wait)
    finally:
        s.close()


def shot_page(qemu, tmp, args):
    """--page 模式：抓「文件资源管理器」的三种页面（此电脑 / 盘内图标 / 详细信息四列）。

    需要一块**带 VimtuFS2 卷的已装系统盘**才好看：默认用 tests/fs_tree_test.py 的 Python 侧
    v3 卷夹具现造一块 16MB 小系统盘（用户显式给了 --img 时就用用户的）。
    导航复用 tests/explorer64_test.py 里已验证过的鼠标模型（park -> 精确位移 -> 单击/双击）：
    键盘只负责开窗（Win+2），进盘与切视图都走真实鼠标点击。
    """
    import sys as _sys
    _sys.path.insert(0, os.path.join(ROOT, "tests"))
    need_fixture = os.path.abspath(args.img) == os.path.abspath(os.path.join(ROOT, "build64", "system.img"))
    img = args.img
    if need_fixture:
        try:
            import fs_tree_test as fst
        except Exception as e:
            sys.stderr.write("--page 需要 tests/fs_tree_test.py 的卷夹具：%s\n" % e)
            return 2
        img = os.path.join(tmp, "small_system.img")
        if fst.make_small_system_disk(img) is None:
            sys.stderr.write("--page：造小系统盘失败（build64/system.img 缺失或过大）\n")
            return 2
        print("[shot] --page 模式：用 Python 造的 16MB 小系统盘（含 VimtuFS2 v3 卷）")

    try:
        import explorer64_test as E
    except Exception as e:
        sys.stderr.write("--page 需要 tests/explorer64_test.py 的鼠标工具：%s\n" % e)
        return 2

    serial = os.path.join(tmp, "page_serial.log")
    ppm = os.path.join(tmp, "page.ppm")
    port = args.port
    vm = E.Vm(qemu, img, port, serial, "Vimtu64-shot-page")
    mon = E.Monitor(port)
    try:
        if not vm.wait_log("[GUI64] ready", 120):
            sys.stderr.write("--page：桌面没起来（等 [GUI64] ready 超时）\n")
            return 1
        print("[shot] 桌面就绪 -> 页面 %s" % args.page)
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=2.5)                       # 菜单序号 1 = 我的电脑 / 文件资源管理器
        if not vm.wait_log("[UI] explorer thispc", 25):
            sys.stderr.write("--page：文件资源管理器没有打开\n")
            return 1
        time.sleep(1.0)
        E.SAFE_POINT = (E.sx(E.CONTENT_X + E.CONTENT_W - 40), E.sy(E.CONTENT_Y + E.CONTENT_H - 40))
        E.ensure_window_on_top(vm, mon)
        if args.page in ("drive", "details"):
            tx, ty = E.card_center(1)                # 第 0 张是不可浏览的 ESP，第 1 张 = C:
            E.aim_click(vm, mon, tx, ty, "card:1")   # 双击进入盘根
            time.sleep(1.5)
        if args.page == "details":
            for _ in range(4):
                if E.last_view(vm) == "details":
                    break
                E.aim_single_click(vm, mon, E.sx(176), E.sy(13), "btn:view")
                time.sleep(2.0)
            print("[shot] 视图 = %s" % (E.last_view(vm) or "?"))
        mon.send("mouse_move 300 150", wait=0.8)     # 光标挪开，别压住窗口内容
        time.sleep(1.0)
        if os.path.exists(ppm):
            os.remove(ppm)
        mon.shot(ppm, wait=3.0)
        if not os.path.exists(ppm):
            sys.stderr.write("--page：没拿到 screendump\n")
            return 1
        out_dir = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(out_dir, exist_ok=True)
        try:
            from PIL import Image   # noqa
            Image.open(ppm).save(args.out)
            print("[shot] 已保存 PNG：%s（%d 字节）" % (args.out, os.path.getsize(args.out)))
        except Exception as e:
            kept = os.path.join(out_dir, os.path.basename(args.out) + ".ppm")
            with open(ppm, "rb") as a, open(kept, "wb") as b:
                b.write(a.read())
            print("[shot] 没有 Pillow（%s），已保留 PPM：%s" % (e, kept))
        return 0
    finally:
        vm.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "screenshots", "desktop64.png"))
    ap.add_argument("--apps", default="4,3,6",
                    help="要打开的**菜单序号**（逗号分隔）：1终端 2我的电脑 3系统监视器 4计算器 5扫雷 6设置 7任务管理器 8关于（别选 0重启/9关机）")
    ap.add_argument("--no-apps", action="store_true")
    ap.add_argument("--port", type=int, default=5599)
    ap.add_argument("--keys", default="",
                    help="打开应用后再注入的 QEMU sendkey 名（逗号分隔），例如 'right,down,down,down' "
                         "把任务管理器切到性能页并把选中项移到\"显卡\"；'1' 把设置页切到\"系统（设备规格）\"")
    ap.add_argument("--page", default="",
                    help="抓文件资源管理器页面：thispc（此电脑）/ drive（盘内图标视图）/ details（详细信息四列）；"
                         "该模式下默认自己造一块带 VimtuFS2 卷的小系统盘")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu()
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_shot_")
    if args.page:
        return shot_page(qemu, tmp, args)
    serial = os.path.join(tmp, "serial.log")
    ppm = os.path.join(tmp, "desktop.ppm")

    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-shot",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ok = False
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                with open(serial, "r", encoding="utf-8", errors="replace") as f:
                    if "[GUI64] ready" in f.read():
                        ok = True
                        break
            except OSError:
                pass
            time.sleep(0.5)
        if not ok:
            sys.stderr.write("桌面没起来（等 [GUI64] ready 超时）\n")
            return 1

        print("[shot] 桌面就绪，%.1fs" % (time.time() - t0))
        if not args.no_apps:
            for s in args.apps.split(","):
                s = s.strip()
                if not s.isdigit():
                    continue
                idx = int(s)
                digit = DIGIT_OF_INDEX.get(idx)
                if not digit:
                    continue
                mon_send(args.port, "sendkey meta_l", wait=1.1)   # 开始菜单
                mon_send(args.port, "sendkey %s" % digit, wait=2.0)
                print("[shot] 已打开菜单序号 %d（按键 %s）" % (idx, digit))
            time.sleep(1.5)

        # 可选的页面导航键（批次 B）：例如 tmgr 性能页 -> 显卡项，或设置页 -> 系统（设备规格）
        if args.keys.strip():
            for k in args.keys.split(","):
                k = k.strip()
                if not k:
                    continue
                mon_send(args.port, "sendkey %s" % k, wait=0.9)
                print("[shot] 注入按键 %s" % k)
            time.sleep(1.2)
        # 把鼠标挪到画面中间偏下，避免光标压在窗口标题上
        mon_send(args.port, "mouse_move 200 120", wait=0.8)
        time.sleep(0.8)
        if os.path.exists(ppm):
            os.remove(ppm)
        mon_send(args.port, "screendump %s" % q(ppm), wait=3.0)
        for _ in range(20):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                break
            time.sleep(0.3)
        if not os.path.exists(ppm):
            sys.stderr.write("没拿到 screendump\n")
            return 1

        out_dir = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(out_dir, exist_ok=True)
        try:
            from PIL import Image   # noqa
            Image.open(ppm).save(args.out)
            print("[shot] 已保存 PNG：%s（%d 字节）" % (args.out, os.path.getsize(args.out)))
        except Exception as e:
            kept = os.path.join(out_dir, "desktop64.ppm")
            with open(ppm, "rb") as a, open(kept, "wb") as b:
                b.write(a.read())
            print("[shot] 没有 Pillow（%s），已保留 PPM：%s" % (e, kept))
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
