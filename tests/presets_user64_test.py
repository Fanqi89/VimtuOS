#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/presets_user64_test.py - ★ v0.5.16：设置里的「预设壁纸 / 预设头像 / 改用户名」真能用

用户报告：
  ① "预设的用户头像和系统壁纸在系统中无法找到也无法在'设置'中使用"
  ② "更改了用户名重启后又变回了更改前的用户名"

修前实测（真点设置界面，坐标取自程序自己的 `[SET64] ctl ... ax= ay=` 打点）：
```
[SET64] wall preset idx=0 n=4 name=1.jpeg
[SET64] wall path=/Wallpapers/1.jpeg ok=0 reason=jpeg/gif not implemented (img64: png/bmp only)
```
三个真因：
  * `wall_set_path()` 里留着 **v0.5.8 之前的过时 JPEG 闸门**（img64 的 decode_jpeg64 早已实现，
    头像那条路当初放开了，**壁纸这条路漏了**）→ /Wallpapers 里第一张是 1.jpeg → 点"预设壁纸"
    永远失败；
  * 失败后 `ui.wall.path` 不变 → 下一轮又选同一张 → **按钮看起来完全没反应**（死循环）；
  * 两个按钮的文字还写着 `用 /logo/kaisi.png`、`用内置图`，提示行也写 kaisi.png →
    用户**在界面上根本找不到"预设壁纸/预设头像"**。
用户名那条：`user_rename()` 的日志无条件打 `persisted=1`（保存失败也说成功）；改名后不同步
`ui.login.last`（锁屏默认选中的就是它，所以重启后一眼看到的还是旧名字）；点「改名」时若输入框
已失焦，旧代码提交的是空串 → `invalid-name`，名字没变而界面只闪一句小字。

判据（每条都能独立证伪）：
  A. 壁纸页有「预设壁纸」按钮，点一次 -> `[SET64] wall preset ... applied=1` +
     `[SET64] wall path=/Wallpapers/... ok=1`，且该文件名在 /Wallpapers 里；
     再点一次必须换成**另一张**（循环不卡死）；重启后 `[GFX64] init` / `[SET64] boot apply`
     显示壁纸来源仍是那张文件（真持久化）。
  B. 头像页点「预设头像」-> `[SET64] avatar preset ... applied=1` + `[USER64] avatar ... persisted=1`；
     重启后锁屏 `[IMG64] load path=/Avatars/...` + `[LOGIN64] login screen shown ... avatar=/Avatars/...`。
  C. 用户名页：**不填名字直接点「改名」** -> `[USER64] rename FAIL reason=empty-input`（不再静默失败）；
     填名字再点 -> `[USER64] rename ok ... persisted=1`；重启后 `[USER64] userdb load ... name=<新名>`
     且锁屏 `user=<新名>`（`ui.login.last` 跟着改）。
  D. 全程无 PANIC。

退出码：0 全过 / 1 有断言失败 / 2 环境问题
"""
import os
import re
import socket
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
WORK = os.path.join(ROOT, "build64", "presets_user64")
TGT = os.path.join(WORK, "target8g.img")
PORT = 5597
NEWNAME = "neo"

P_WALL, P_AVATAR, P_UNAME = 5, 12, 13
CID_WALL_PRESET, CID_AVATAR_PRESET = 73, 147
CID_UNAME_FIELD, CID_UNAME_APPLY = 150, 151

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
    a = [QEMU, "-name", "presets_user64", "-m", "512", "-vga", "std", "-display", "none",
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
    """控件中心点的绝对屏幕坐标（ax+w/2, ay+h/2）。见下面 find_nav 的注释。"""
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


def do_install():
    if os.path.exists(TGT):
        return True
    os.makedirs(WORK, exist_ok=True)
    with open(TGT, "wb") as f:
        f.truncate(8 * 1024 ** 3)
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
        return wait_in(serial, "[SETUP] 安装完成", 1800)
    finally:
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()


def open_settings(mon, serial):
    """点 Dock 里的"设置"（[DOCK64] item idx=6 cx=754 cy=754），等 nav 打点。"""
    cur = SM.Cursor()
    cur.goto(mon, 754, 754)
    mon.click(wait=2.5)
    return wait_for(lambda: nav_center(log_of(serial), P_WALL), 40), cur


def main():
    if not QEMU or not os.path.exists(MEDIUM):
        print("跳过：qemu / 安装介质不可用")
        return 2
    if not do_install():
        print("安装没完成")
        return 2

    # ================= 第一轮：设置界面真点 =================
    serial = os.path.join(WORK, "ui1.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(TGT), "-boot", "order=c"], serial, PORT)
    try:
        if not qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=400):
            print("第一次启动没进桌面")
            return 2
        nav, cur = open_settings(mon, serial)
        check("设置能打开（导航打点存在，壁纸页坐标=%s）" % (nav,), nav is not None)
        if not nav:
            return 1

        # ---- A. 预设壁纸 ----
        cur.goto(mon, nav[0], nav[1])
        mon.click(wait=2.0)
        btn = wait_for(lambda: ctl_center(log_of(serial), P_WALL, CID_WALL_PRESET), 30)
        check("壁纸页有「预设壁纸」按钮（id=%d 已登记 %s）" % (CID_WALL_PRESET, btn), btn is not None)
        walls = []
        if btn:
            for i in range(4):                      # n=4：把 /Wallpapers 里每一张都走一遍
                cur.goto(mon, btn[0], btn[1])
                mon.click(wait=3.0)
                time.sleep(2.0)
                m = re.findall(r"\[SET64\] wall path=(/Wallpapers/[^\s]+) ok=1", log_of(serial))
                walls.append(m[-1] if m else None)
                btn = ctl_center(log_of(serial), P_WALL, CID_WALL_PRESET) or btn
        log1 = log_of(serial)
        check("点「预设壁纸」真的应用成功（[SET64] wall path=/Wallpapers/... ok=1）",
              walls and walls[0] is not None, str(walls[0]))
        check("预设壁纸循环不卡死（第二次点换成另一张）",
              len(walls) >= 2 and walls[1] is not None and walls[1] != walls[0],
              "%s -> %s" % (walls[0] if walls else "?", walls[1] if len(walls) > 1 else "?"))
        check("应用的是 /Wallpapers 里的文件", bool(walls) and walls[0] and walls[0].startswith("/Wallpapers/"),
              str(walls[0] if walls else ""))
        check("壁纸应用打点带 applied=1（成功路径）",
              re.search(r"\[SET64\] wall preset idx=\d+ n=\d+ name=\S+ applied=1", log1) is not None)
        # ★ 用户自己的 JPEG 壁纸（/Wallpapers/1.jpeg）也必须能用：修前被一条过时的
        #   "jpeg/gif not implemented (img64: png/bmp only)" 闸门挡住，而 img64 其实早就能解 JPEG。
        check("JPEG 壁纸也能应用（[SET64] wall path=/Wallpapers/1.jpeg ok=1）",
              re.search(r"\[SET64\] wall path=/Wallpapers/1\.jpeg ok=1", log1) is not None,
              "已应用：%s" % [w for w in walls if w])

        # ---- B. 预设头像 ----
        nav12 = wait_for(lambda: nav_center(log_of(serial), P_AVATAR), 10)
        abtn = None
        if nav12:
            cur.goto(mon, nav12[0], nav12[1])
            mon.click(wait=2.0)
            abtn = wait_for(lambda: ctl_center(log_of(serial), P_AVATAR, CID_AVATAR_PRESET), 25)
        check("头像页有「预设头像」按钮（id=%d %s）" % (CID_AVATAR_PRESET, abtn), abtn is not None)
        if abtn:
            cur.goto(mon, abtn[0], abtn[1])
            mon.click(wait=3.0)
            time.sleep(2.0)
        log2 = log_of(serial)
        m = re.search(r"\[USER64\] avatar user=\S+ src=(/Avatars/\S+) via=settings px=\d+x\d+ "
                      r"saved=1 persisted=1", log2)
        check("点「预设头像」写入 users.db 且 persisted=1", m is not None,
              (m.group(0) if m else "缺失"))

        # ---- C. 用户名：空输入必须明确报错；填名字后改名成功 ----
        nav13 = wait_for(lambda: nav_center(log_of(serial), P_UNAME), 10)
        fld = ubtn = None
        if nav13:
            cur.goto(mon, nav13[0], nav13[1])
            mon.click(wait=2.0)
            fld = wait_for(lambda: ctl_center(log_of(serial), P_UNAME, CID_UNAME_FIELD), 25)
            ubtn = wait_for(lambda: ctl_center(log_of(serial), P_UNAME, CID_UNAME_APPLY), 25)
        check("用户名页有输入框(id=150 %s)与「改名」按钮(id=151 %s)" % (fld, ubtn),
              fld is not None and ubtn is not None)
        if fld and ubtn:
            cur.goto(mon, ubtn[0], ubtn[1])          # 先不填名字直接点
            mon.click(wait=2.0)
            time.sleep(1.0)
            check("空输入点「改名」会明确报错（reason=empty-input，不再静默失败）",
                  "rename FAIL reason=empty-input" in log_of(serial))
            cur.goto(mon, fld[0], fld[1])
            mon.click(wait=1.5)
            P.type_line(mon, NEWNAME)
            time.sleep(1.0)
            cur.goto(mon, ubtn[0], ubtn[1])
            mon.click(wait=3.0)
            time.sleep(2.0)
        log3 = log_of(serial)
        m = re.search(r"\[USER64\] rename ok from=(\S+) to=(\S+) .*persisted=1", log3)
        check("改名成功且 persisted=1（from=%s to=%s）" % (m.group(1), m.group(2)) if m else "改名成功",
              m is not None)
        check("改名后同步了 ui.login.last（[STORE64] set cfg.ui.login.last=%s）" % NEWNAME,
              ("cfg.ui.login.last=" + NEWNAME) in log3)
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
        except Exception:
            proc.kill()
    finally:
        if proc.poll() is None:
            proc.kill()

    # ================= 第二轮：重启，逐个确认持久化 =================
    serial2 = os.path.join(WORK, "ui2.log")
    if os.path.exists(serial2):
        os.remove(serial2)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(TGT), "-boot", "order=c"], serial2, PORT)
    try:
        up2 = qh.wait_desktop(mon, lambda: log_of(serial2), proc, timeout=400)
        log4 = log_of(serial2)
        check("重启后仍进桌面", up2)
        check("重启后用户表里是新名字（userdb load … name=%s）" % NEWNAME,
              re.search(r"\[USER64\] userdb load line=\d+ name=%s " % NEWNAME, log4) is not None,
              (re.search(r"\[USER64\] userdb load line=\d+ name=\S+[^\r\n]*", log4) or [""])[0].strip())
        check("锁屏默认选中的也是新名字（ui.login.last 跟随）",
              re.search(r"\[LOGIN64\] login screen shown[^\r\n]*user=%s" % NEWNAME, log4) is not None,
              (re.search(r"\[LOGIN64\] login screen shown[^\r\n]*user=\S+", log4) or [""])[0].strip()[:110])
        check("重启后头像仍是预设图（[IMG64] load path=/Avatars/… ok=1）",
              re.search(r"\[IMG64\] load path=/Avatars/\S+ ok=1", log4) is not None,
              (re.search(r"\[IMG64\] load path=/Avatars/\S+[^\r\n]*", log4) or [""])[0].strip()[:110])
        check("重启后壁纸仍是预设文件（[SET64] boot apply ... wall=… 或 [GFX64] init … src=vfs:wallpaper）",
              re.search(r"wall=/Wallpapers/|src=vfs:wallpaper", log4) is not None,
              (re.search(r"\[SET64\] boot apply[^\r\n]*", log4) or [""])[0].strip()[:110])
        check("全程无 PANIC", "PANIC" not in log4)
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
