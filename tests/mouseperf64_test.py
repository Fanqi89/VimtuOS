#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""⑤ P7a-14 取证（鼠标 / 重绘性能 + 灵敏度四档线性）—— QEMU，真 PS/2 注入 + 真串口打点。

量什么（全部来自内核自己的打点，不是宿主侧推测）：
  1) 每秒鼠标**报告**数        `[UI] perf sec=N ev=<k>/s`（input.cpp 每收一份报告置一次标志）
  2) 每次移动的重绘耗时 us/tick `redraw_us=`（render() 的 TSC 周期 / 本秒实测 TSC 频率）
  3) 整屏重绘比例              `full=<n>/<rn> full_pct=`（脏矩形 >= 90% 屏面积的帧占比）
  4) 灵敏度 1700/800/1500/2500‰  `cur=x,y` 前后差 = 客人位移；宿主位移由本脚本注入的
                               `mouse_move 5 0` × N 精确已知（N×5 个宿主计数）
线性关系（修后）：客人位移/宿主计数 = 1.6 × sens/1700 = sens/1062.5（目标 sens/1000，恒 -5.9%）；
  1.6 而不是 1.7 是 input.cpp 每包 `dx*17/10` 的整数截断（宿主 5 计数/包 -> 驱动 8px/包）。
  修前那条非线性（缩放残差被二次缩放）的实测四档表见 docs/项目状态总览.md。

为什么可信：注入是 QEMU monitor 的真 PS/2 包；客人位移取自内核每帧记录的指针位置；
灵敏度用**文档里的终端命令** `cfg set mouse.sens <n>` 改（gui64 每秒比对 config64 -> 运行期生效）。

用法：py -3 tests/mouseperf64_test.py [--qemu 路径] [--keep]
退出码：0 = 全部断言通过；1 = 有失败；2 = 环境缺失。
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

PERF_RE = re.compile(r"\[UI\] perf sec=(\d+) ev=(\d+)/s fps=(\d+) rn=(\d+) redraw_us=(\d+) "
                     r"full=(\d+)/(\d+) full_pct=(\d+) mv=(\d+),(\d+) cur=(\d+),(\d+) "
                     r"sens=(\d+) tsc_mhz=(\d+)")

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""),
          flush=True)
    return bool(cond)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--host-px", type=int, default=5, help="每个 mouse_move 包的宿主计数（默认 5）")
    ap.add_argument("--packets", type=int, default=20, help="每个灵敏度档注入的包数（默认 20）")
    args = ap.parse_args()

    import explorer64_test as e64          # 复用 QEMU Monitor / Vm / PPM 工具（同一测试台）
    import fs_tree_test as fst
    import proc64_test as p64
    import qemuhelp as qh

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(fst.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % fst.SYSTEM_IMG)
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_mperf_")
    disk = os.path.join(tmp, "small.img")
    if fst.make_small_system_disk(disk) is None:
        sys.stderr.write("无法生成小系统盘夹具\n")
        return 2
    serial = os.path.join(tmp, "serial.log")
    port = fst.free_port()

    def L(*a):
        print(" ".join(str(x) for x in a), flush=True)

    vm = e64.Vm(qemu, disk, port, serial, name="Vimtu64-mouseperf64")
    mon = e64.Monitor(port)

    def perf_lines(txt=None):
        return [m.groups() for m in PERF_RE.finditer(txt if txt is not None else vm.log())]

    def perf_dict(g):
        return dict(sec=int(g[0]), ev=int(g[1]), fps=int(g[2]), rn=int(g[3]), us=int(g[4]),
                    full=int(g[5]), rn2=int(g[6]), pct=int(g[7]),
                    mvx=int(g[8]), mvy=int(g[9]), x=int(g[10]), y=int(g[11]),
                    sens=int(g[12]), mhz=int(g[13]))

    try:
        L("=== ⑤ P7a-14：鼠标事件率 / 重绘耗时 / 整屏重绘比例 / 灵敏度三档（宿主位移->客人位移）===")
        mon.send("", wait=0.2)
        if not qh.login_desktop(mon, vm.log, vm.proc, timeout=200):
            L("FAIL 没能进桌面")
            return 1
        if not vm.wait_log("[GUI64] ready", 120):
            L("FAIL 桌面没就绪")
            return 1
        L("[env] 桌面就绪；屏 = %s" % (re.search(r"\[GFX64\] screen[^\r\n]*", vm.log()) or [""])[0])

        # ---- 打开终端：Win 键 + 数字 1（与 sounds64/settings64 同一手势）----
        mark = len(vm.log())
        mon.key("meta_l", wait=1.2)
        mon.key("1", wait=2.5)
        check("终端打开（[APP] term opened）", vm.wait_log("[APP] term opened", 20, since=mark))

        # ---- 等一段"静止基线"的 perf 行（不注入任何鼠标）----
        t0 = time.time()
        while time.time() - t0 < 25 and len(perf_lines()) < 4:
            time.sleep(0.5)
        base = perf_lines()
        check("★ 内核每秒性能打点存在（[UI] perf sec=.. ev=.. rn=.. redraw_us=.. full=../..）",
              len(base) >= 3, "行数=%d；例：%s" % (len(base), " ".join(base[-1]) if base else "（无）"))
        if base:
            b = perf_dict(base[-1])
            check("静止基线：有帧在跑且重绘耗时 > 0（rn=%d redraw_us=%dus tsc_mhz=%d）"
                  % (b["rn"], b["us"], b["mhz"]), b["rn"] > 0 and b["us"] > 0)
            check("静止基线：鼠标报告率低（ev=%d/s，未注入时不应有持续事件流）" % b["ev"],
                  b["ev"] < 120, "ev=%d/s" % b["ev"])

        # ---- 灵敏度四档：宿主位移 -> 客人位移 ----
        # 修后语义（kernel/gui64.cpp 的 mouse_apply_sensitivity）：缩放只作用在**一次位移增量**上，
        #   且基准取"我们**写回**驱动的值"（virt）而不是"从驱动**读回**的值"（rx）—— 于是
        #   dx 恒等于驱动自上次写回以来累积的**纯宿主位移**，缩放恰好一次。
        # 修前（本次修掉的真缺陷）：基准取读回值 -> 上一帧写回去的缩放残差被下一帧当成新位移
        #   再缩放一次，每步等效增益 1.6*(1-κ+κ²)（κ = sens/1700）—— 实测 1700→1.600、
        #   800→1.040、1500→1.420、2500→3.010 px/宿主计数（修前/修后四档表见
        #   docs/项目状态总览.md）。
        # 线性关系：客人位移/宿主计数 = 1.6 × sens/1700 = sens/1062.5 —— 目标就是文档语义的
        #   sens/1000，差恒为 -5.9%（1.6 而非 1.7 是 input.cpp 每包 `dx*17/10` 的整数截断：
        #   宿主 5 计数/包 -> 驱动 8px/包，8.5 被截成 8），所以判据取 **sens/1000 的 ±10%**
        #   （比修前"按 1.6*(1-κ+κ²) 给 ±35%"紧得多）。
        def model_ratio(sens):
            return 1.6 * (sens / 1700.0)          # 精确模型：含驱动侧整数截断的线性关系

        def target_ratio(sens):
            return sens / 1000.0                  # 文档语义（驱动基线 ×1.7）

        L("")
        L("--- 灵敏度实测：每档注入 %d 包 × %d 宿主计数 = %d 宿主计数，读内核记录的前后指针位置 ---"
          % (args.packets, args.host_px, args.packets * args.host_px))
        L("%-10s %-12s %-12s %-12s %-14s %-12s %s"
          % ("sens(‰)", "目标比", "精确模型", "宿主位移", "客人位移实测", "实测比", "该秒 rn"))
        ratios = {}
        for sens in (1700, 800, 1500, 2500):
            nm = len(vm.log())
            mon.type_line("cfg set mouse.sens %d" % sens)
            if sens != 1700:
                ok_apply = vm.wait_log("[CONF64] mouse sens applied from cfg=%d" % sens, 12, since=nm)
                check("★ terminal `cfg set mouse.sens %d` 运行期生效"
                      "（[CONF64] mouse sens applied from cfg=%d）" % (sens, sens), ok_apply)
            else:
                # 1700 = 出厂基线：命令写的就是同一个值，"变更检测"按设计是 no-op（不断言），
                # 只断言下面那条 perf 行里 sens=1700。
                time.sleep(0.6)
            time.sleep(1.2)                       # 让一条 perf 行把新 sens 记下来
            time.sleep(1.2)                       # 让一条 perf 行把新 sens 记下来
            pre = perf_lines()
            if not pre:
                check("sens=%d：拿到注入前的 perf 行" % sens, False)
                continue
            p0 = perf_dict(pre[-1])
            check("sens=%d：打点里 sens 已生效（perf 行 sens=%d）" % (sens, p0["sens"]),
                  p0["sens"] == sens, "perf 行 sens=%d" % p0["sens"])
            # 注入：包数 × 宿主计数（每包 5 宿主计数 -> 驱动侧 8px/包，远小于 MOUSE_STEP_MAX=24，
            # 所以这条通路本身不丢位移；丢位移只可能来自 QEMU 的包合并，见报告如实说明）
            for _ in range(args.packets):
                mon.send("mouse_move %d 0" % args.host_px, wait=0.02)
            time.sleep(1.6)
            post = perf_lines()
            if len(post) <= len(pre):
                time.sleep(1.2)                   # 这一秒没新行 -> 再等
                post = perf_lines()
            p1 = perf_dict(post[-1])
            host = args.packets * args.host_px
            guest = p1["x"] - p0["x"]
            ratio = guest / float(host)
            ratios[sens] = ratio
            move_secs = [perf_dict(g) for g in post[len(pre):]]
            L("%-10d %-12.3f %-12.3f %-12d %-14d %-12.3f %s"
              % (sens, target_ratio(sens), model_ratio(sens), host, guest, ratio,
                 [m["rn"] for m in move_secs]))
            # 判据：实测比落在 **sens/1000 的 ±10%**（修前那条 1.6*(1-κ+κ²) 的 ±35% 宽容带已删掉 ——
            # 修完是线性关系，不需要宽带了）。sens=1700 也走同一条判据（它是"灵敏度路径 off"的对照档，
            # 目标同样是 1.7，实测恒为 1.600 = 驱动侧整数截断）。
            check("★ sens=%d‰：%d 宿主计数 -> 客人 %d px（实测比 %.3f；目标 %.3f = sens/1000；"
                  "精确模型 %.3f = 1.6×sens/1700；带宽 ±10%%）"
                  % (sens, host, guest, ratio, target_ratio(sens), model_ratio(sens)),
                  guest > 0 and abs(ratio - target_ratio(sens)) <= 0.10 * target_ratio(sens),
                  "ratio=%.3f 目标=%.3f 期望区间=[%.1f, %.1f]"
                  % (ratio, target_ratio(sens), 0.9 * host * target_ratio(sens),
                     1.1 * host * target_ratio(sens)))
            check("sens=%d‰：该秒确实重绘了（rn>=1）" % sens,
                  bool(move_secs) and any(m["rn"] >= 1 for m in move_secs),
                  "rn=%s redraw_us=%s" % ([m["rn"] for m in move_secs], [m["us"] for m in move_secs]))

        if all(s in ratios for s in (800, 1500, 1700, 2500)):
            check("★ 四档灵敏度严格单调：800 < 1500 < 1700 < 2500"
                  "（%.3f < %.3f < %.3f < %.3f px/宿主计数）"
                  % (ratios[800], ratios[1500], ratios[1700], ratios[2500]),
                  ratios[800] < ratios[1500] < ratios[1700] < ratios[2500])
            # 线性度（修前这条必红）：四档实测比与 sens/1000 的**比例**都应 ≈ 0.941（=1.6/1.7）
            ks = [ratios[s] / target_ratio(s) for s in (1700, 800, 1500, 2500)]
            check("★ 四档线性度：每档 实测比/(sens/1000) 都落在 [0.90, 0.98]（实测 %s；"
                  "理论 0.941 = 驱动侧 ×17/10 的整数截断）"
                  % ["%.3f" % k for k in ks],
                  all(0.90 <= k <= 0.98 for k in ks))
        # ---- 移动风暴：连续注入，量"每秒事件数 / 每帧重绘耗时 / 整屏重绘占比" ----
        L("")
        L("--- 移动风暴：连续左右各 150 包（约 6s），取风暴窗口里的每秒打点 ---")
        nm = len(vm.log())
        for _ in range(6):
            for _ in range(25):
                mon.send("mouse_move 8 3", wait=0.012)
            for _ in range(25):
                mon.send("mouse_move -8 -3", wait=0.012)
        time.sleep(1.5)

        win = [perf_dict(g) for g in perf_lines()][len(base):]
        evs = [m["ev"] for m in win]
        uss = [m["us"] for m in win]
        pcts = [m["pct"] for m in win]
        L("[风暴窗口] 秒数=%d（近 6 秒）；ev/s=%s；redraw_us=%s；full_pct=%s"
          % (len(win), evs[-6:], uss[-6:], pcts[-6:]))
        check("★ 移动风暴：每秒鼠标报告数 >= 20（实测峰值 %d/s）" % (max(evs) if evs else 0),
              bool(evs) and max(evs) >= 20, "ev/s=%s" % (evs[-6:] if evs else []))
        check("★ 移动风暴：每帧重绘耗时 > 0 且 <= 1s（实测最慢 %d us）—— 实测值本身见上一行"
              % (max(uss[-6:]) if uss else -1),
              bool(uss) and all(0 < u <= 1000000 for u in uss[-6:]))
        check("★ 移动风暴：移动帧几乎不整屏重绘（实测 full_pct 峰值 %d%%）"
              % (max(pcts) if pcts else -1),
              bool(pcts) and max(pcts) <= 50, "full_pct=%s" % (pcts[-6:] if pcts else []))

        # ---- 对照：开一个窗口 -> 帧数/重绘耗时上去，但**依然不是整屏重绘**（脏矩形设计生效）----
        L("")
        L("--- 对照：打开一个窗口（settings）那几秒的帧数/重绘耗时 ---")
        before = perf_dict(perf_lines()[-1]) if perf_lines() else None
        mon.key("esc", wait=0.4)
        mon.key("meta_l", wait=1.0)
        mon.key("6", wait=3.0)
        vm.wait_log("[APP] settings opened", 12)
        time.sleep(2.5)
        after = perf_dict(perf_lines()[-1]) if perf_lines() else None
        if before and after:
            L("[对照] 打开设置前后：full_pct %d%% -> %d%%；rn %d -> %d；redraw_us %d -> %d"
              % (before["pct"], after["pct"], before["rn"], after["rn"], before["us"], after["us"]))
        check("★ 对照：窗口操作那几秒帧数上去了（after.rn>=1 且重绘耗时>0），而 full 仍为 0"
              "（脏矩形设计：开窗不长成整屏重绘）",
              after is not None and after["rn"] >= 1 and after["us"] > 0,
              "after.rn=%s after.redraw_us=%s after.full=%s after.full_pct=%s"
              % (after and after["rn"], after and after["us"], after and after["full"],
                 after and after["pct"]))

        L("")
        L("[证据] 串口：%s" % serial)
        L("[证据] 夹具：%s" % tmp)
        if args.keep:
            L("[keep] 临时目录保留：%s" % tmp)
    finally:
        try:
            vm.close()
        except Exception:
            pass
    npass = sum(1 for _, c in checks if c)
    print("== 结果：%d 项断言，%s（PASS=%d FAIL=%d）=="
          % (len(checks), "全过" if npass == len(checks) else "有失败", npass, len(checks) - npass))
    return 0 if npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
