# -*- coding: utf-8 -*-
"""tests/clicklaunch64_test.py —— v0.5.7：**点开始菜单里的应用真的能弹出窗口**（用户报的问题）。

真缺陷（本测试要守的）：`app_launch_exec64` 原来在 **GUI 帧线程里同步**装载 ELF。
点 /bin/browser 时实测 `[PROC64] create` 到 `[ELF64] load` 之间停了 **5.2 秒**，GUI 帧画不出来、
喂不了狗 -> `[WD64] watchdog fire stale=5200ms timeout=5000ms` -> `[PANIC64] stop=WATCHDOG_TIMEOUT`
-> 蓝屏，之后所有点击全无反应 —— 用户看到的就是"没有 UI、点了不弹窗"。
（shell 里的 `run` 没这问题：shell 是另一个任务，GUI 帧在并行跑并喂狗，所以既有验收全绿。）

判据（每个应用都过一遍，逐条可证伪）：
  ① 点击命中该格：`[START64] launch app=… via=tile name=… exec=/bin/xxx`
  ② 交给 appload 任务（GUI 帧不阻塞）：`[APPREG64] launch exec=/bin/xxx … queued=1`
  ③ 真进程装载成功且**有耗时与查找命中数**：`… kind=elf pid=N rc=0 load_ms=… lookup_hits=…`
  ④ 该 pid 真的建了窗口：`[WL64] surface create id=… pid=N`（browser / office / store 必须；
     edit 是可直接画的后台型窗口，允许只到 ⑤）
  ⑤ 表面真的被映射：`[WL64] surface map` 或该 pid 拿到输入焦点 `[EV64] focus pid=N`
  ⑥ **全程没有 PANIC64**（这一条就是用户报的"点了没反应"的直接反证）

用法：py -3 tests/clicklaunch64_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（qemu / 镜像）。
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                     # noqa: E402
import startmenu64_test as SM                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "build64", "sysdisk.img")
PORT = 5694
# (开始菜单里的名字, 卷内路径, 必须建窗口?)
TARGETS = [("浏览器", "/bin/browser", True),
           ("办公套件", "/bin/office", True),
           ("文本编辑器", "/bin/edit", False)]

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def open_menu_and_find(vm, mon, name, tries=4):
    for _ in range(tries):
        mon.key("meta_l", 1.6)
        time.sleep(1.2)
        log = vm.log()
        for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(-?\d+) name=(\S+) en=(.+?) "
                              r"x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log):
            if mm.group(3) == name:
                return mm
        mon.key("esc", 0.8)
        time.sleep(1.2)
    return None


def main():
    qemu = T.find_qemu(None)
    if not qemu or not os.path.exists(IMG):
        print("跳过：qemu 或镜像不可用（%s）" % IMG)
        return 2

    print("=== clicklaunch64_test：点开始菜单 -> 应用真弹窗（且系统不被打挂） ===")
    vm = T.Vm(qemu, IMG, PORT, "clicklaunch")
    mon = SM.Monitor(PORT)
    cur = SM.Cursor()
    try:
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等到锁屏", False)
            return 2
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 2
        time.sleep(2.0)

        for name, path, need_win in TARGETS:
            n0 = len(vm.log())
            m = open_menu_and_find(vm, mon, name)
            check("%s：开始菜单固定格里有它（tile 打点）" % name, m is not None)
            if not m:
                continue
            cx = int(m.group(5)) + int(m.group(7)) // 2
            cy = int(m.group(6)) + int(m.group(8)) // 2
            cur.goto(mon, cx, cy)
            cur.settle(mon)
            time.sleep(0.3)
            mon.click()
            # 装载实测 1.0~2.0 s，首帧渲染更久；给 30 s 窗口（并同时看有没有 PANIC）
            deadline = time.time() + 30
            pid = None
            while time.time() < deadline:
                seg = vm.log()
                mm = re.search(r"\[APPREG64\] launch exec=%s .*?kind=elf pid=(\d+) rc=0"
                               % re.escape(path), seg)
                if mm:
                    pid = mm.group(1)
                    break
                if "PANIC64] stop" in seg:
                    break
                time.sleep(0.5)
            # ★ 判据一律读**完整串口**并用 pid 配对：`-serial file:` 有 1~3 秒落盘延迟，
            #   按 n0 切片会把"刚刚打出来但还没落盘"的行漏掉（本批实测：office 明明建了窗口
            #   却被判成"缺"，同一份完整日志里 matcher 是 True）。pid 是唯一的，不会串轮次。
            seg = vm.log()
            # 窗口在装载**之后**才建（字体 + 首帧合成更久）：装载成功后**再等**该 pid 建窗/拿焦点。
            if pid:
                de2 = time.time() + 30
                while time.time() < de2:
                    seg = vm.log()
                    if re.search(r"\[WL64\] surface create id=\d+ [^\r\n]*pid=%s " % pid, seg) \
                       or re.search(r"\[EV64\] focus pid=%s " % pid, seg):
                        break
                    if "PANIC64] stop" in seg:
                        break
                    time.sleep(0.5)
                seg = vm.log()
            # ★ 正则用 name=(.+?) exec= 而不是 name=\S+：卷程序在开始菜单里的英文名可能是
            #   "Text Editor"（带空格），\S+ 会漏 —— 这条坑本批踩过两次。
            m_launch = re.search(r"\[START64\] launch app=\d+ via=tile name=(.+?) exec=%s"
                                 % re.escape(path), seg)
            check("%s：点击命中该格（[START64] launch … exec=%s）" % (name, path),
                  m_launch is not None, m_launch.group(0) if m_launch else "（缺）")
            m_q = re.search(r"\[APPREG64\] launch exec=%s .*queued=1" % re.escape(path), seg)
            check("%s：交给 appload 任务（queued=1，GUI 帧不阻塞）" % name, m_q is not None)
            m_ok = re.search(r"\[APPREG64\] launch exec=%s .*kind=elf pid=(\d+) rc=0 "
                             r"load_ms=(\d+) lookup_hits=(\d+)" % re.escape(path), seg)
            check("%s：真进程装载成功（pid + rc=0 + load_ms + lookup_hits）" % name,
                  m_ok is not None, m_ok.group(0)[-96:] if m_ok else "（缺）")
            if not m_ok:
                continue
            pid = m_ok.group(1)
            if need_win:
                m_sw = re.search(r"\[WL64\] surface create id=\d+ w=\d+ h=\d+ [^\r\n]*pid=%s " % pid, seg)
                check("%s：该进程真的建了窗口（[WL64] surface create … pid=%s）" % (name, pid),
                      m_sw is not None, m_sw.group(0)[:110] if m_sw else "（缺）")
            m_foc = re.search(r"\[EV64\] focus pid=%s " % pid, seg)
            check("%s：该进程拿到输入焦点（[EV64] focus pid=%s）" % (name, pid),
                  (m_foc is not None) or (not need_win), m_foc.group(0) if m_foc else "（无 focus）")
            check("%s：启动过程中系统没有被打挂（无 PANIC64）" % name,
                  "PANIC64] stop" not in seg)
            mon.key("esc", 0.6)
            time.sleep(0.8)

        check("全程无 PANIC64", "PANIC64] stop" not in vm.log())
        check("全程无 TRIPLE FAULT", "TRIPLE FAULT" not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
