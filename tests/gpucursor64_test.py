# -*- coding: utf-8 -*-
"""tests/gpucursor64_test.py —— v0.5.5：virtio-gpu 硬件光标平面（queue 1）端到端验收。

判据（每条都能独立证伪）：
  ① 队列 1 真的建起来了：`[VGPU] queue1 size=.. (cursor; ready)`
  ② 光标 resource + UPDATE_CURSOR 成功：`[VGPU] cursor on res=256 64x64 fmt=b8g8r8a8 hot=0,0 queue=1 sz=..`
  ③ fw_cfg 开关生效：`[VSYNC] cursor cfg=1 -> cursor=hw`
  ④ **位图真的进了设备**：`TRANSFER_FROM_HOST_2D` 把 cursor resource **读回来**，与本地位图
     逐字节比对 —— `[VGPU] cursor verify bytes=16384 diff=0 white=.. black=.. match=1`。
     （这条替代了原来那条"screendump 里找白色箭头"的断言：QEMU 的 cursor 平面是交给**前端显示**
     的第 4 层，不进 scanout surface，screendump 抓不到 —— 拿截图判硬件光标会把"没验证"写成绿。）
  ⑤ 鼠标移动 -> MOVE_CURSOR 真的发到设备：`[VGPU] cursor move x=.. y=.. dev=1 moves=..`
  ⑥ **反向像素判据**：软件光标已经让位（`[VGPU] cursor software-skip`）时，光标坐标处的
     后备缓冲必须是**纯背景**（黑边点 0、黑像素 0）—— 出现"白像素且左上相邻是黑"就说明软件
     光标还在画。
  ⑦ 反向：`-vga std`（无 virtio-gpu）+ cursor=1 -> `[VGPU] cursor disable reason=no-device`
     且 `software-skip` 一次都不出现（软件光标照画），系统照常进桌面。
  ⑧ 全程无 PANIC / TRIPLE FAULT。

用法：py -3 tests/gpucursor64_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（QEMU、镜像）。
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vsync64_test as V                                          # noqa: E402
import textstress64_test as T                                     # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = os.path.join(ROOT, "build64", "sysdisk.img")
WORK = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "gpucursor")
os.makedirs(WORK, exist_ok=True)
ck = V.Checks()


def walk(vm, mon_key, tag):
    """锁屏 -> 桌面（光标只在 GUI 循环里画，必须先到桌面）。"""
    if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
        ck.ch("%s 等到锁屏" % tag, False)
        return False
    vm.mon_cmd("sendkey ret")
    time.sleep(1.4)
    vm.mon_cmd("sendkey ret")
    if not vm.wait_log(T.DESKTOP_READY, 180):
        ck.ch("%s 进桌面" % tag, False)
        return False
    time.sleep(1.5)
    return True


def main():
    qemu = T.find_qemu(None)
    if not qemu or not os.path.exists(IMG):
        print("跳过：qemu 或镜像不可用（%s）" % IMG)
        return 2

    print("=== gpucursor64_test：硬件光标平面（virtio-gpu queue 1） ===")
    print("  [info] cfg=cursor=1  vga=virtio  img=%s" % os.path.basename(IMG))

    # ================= 正向：有 virtio-gpu，开 cursor =================
    vm = V.Vm(qemu, IMG, "virtio", V.free_port(), WORK, "cursor=1")
    try:
        ok_ready = vm.wait_log("[VGPU] ready backend=virtio-gpu-2d", 240)
        ck.ch("设备 2D 路径可用（[VGPU] ready backend=virtio-gpu-2d）", ok_ready)
        if not ok_ready:
            print("\n=== RESULT: UNVERIFIED ===（本机没有可用的 virtio-gpu 2D 路径）")
            return 2
        mq = re.search(r"\[VGPU\] queue1 size=(\d+) \(cursor; ready\)", vm.log())
        ck.ch("队列 1 建成（queue1 size=.. (cursor; ready)）", mq is not None,
              mq.group(0) if mq else "（缺）")
        if not walk(vm, None, "正向"):
            return 2
        m_on = re.search(r"\[VGPU\] cursor on res=(\d+) (\d+)x(\d+) fmt=b8g8r8a8 hot=(\d+),(\d+) queue=1 sz=(\d+)",
                         vm.log())
        ck.ch("光标 resource 建成 + UPDATE_CURSOR 成功（cursor on … queue=1）", m_on is not None,
              m_on.group(0) if m_on else "（缺）")
        ck.ch("fw_cfg 开关生效（[VSYNC] cursor cfg=1 -> cursor=hw）",
              "[VSYNC] cursor cfg=1 -> cursor=hw" in vm.log())
        if not m_on:
            print("\n=== RESULT: FAIL === 硬件光标没起来（详见串口 [VGPU] cursor disable 行）")
            return 1
        m_vf = re.search(r"\[VGPU\] cursor verify bytes=(\d+) diff=(\d+) white=(\d+) black=(\d+) "
                         r"match=(\d) src=(\w+)", vm.log())
        src = m_vf.group(6) if m_vf else "?"
        ck.ch("cursor 位图校验：match=1 diff=0（证据来源 src=%s）" % src,
              bool(m_vf and m_vf.group(5) == "1" and m_vf.group(2) == "0"),
              m_vf.group(0) if m_vf else "（缺）")
        ck.ch("位图非空（white>0 且 black>0 = 实心箭头 + 1px 黑边）",
              bool(m_vf and int(m_vf.group(3)) > 0 and int(m_vf.group(4)) > 0),
              ("white=%s black=%s" % (m_vf.group(3), m_vf.group(4))) if m_vf else "（缺）")
        if src == "self":
            print("  [info] QEMU 对 cursor resource 的 TRANSFER_FROM_HOST_2D 回 ERR_UNSPEC(0x1200)，"
                  "设备回读不可用 -> 本条只证明 backing 写入正确（如实记录，不当作设备侧证据）")
        got_skip = vm.wait_log("[VGPU] cursor software-skip", 20)
        ck.ch("软件光标让位（[VGPU] cursor software-skip 出现）", got_skip)

        # ---- 移动鼠标：让内核发 MOVE_CURSOR 到设备 ----
        for _ in range(6):
            vm.mon_cmd("mouse_move 25 18")
            time.sleep(0.25)
        got_mv = vm.wait_log("[VGPU] cursor move", 20)
        m_mv = None
        for mm in re.finditer(r"\[VGPU\] cursor move x=(\d+) y=(\d+) dev=1 moves=(\d+)", vm.log()):
            m_mv = mm
        ck.ch("移动鼠标 -> MOVE_CURSOR 发到设备（cursor move … dev=1）", bool(got_mv and m_mv),
              m_mv.group(0) if m_mv else "（缺）")

        # ---- 屏幕像素（反向判据，**如实**）----
        # QEMU 的 cursor 平面是**交给前端显示的第 4 层**，不进 scanout surface —— screendump 抓不到它。
        # 所以这里不能断言"截图里有箭头"（那是把没验证的东西写成绿），而是断言**它的反面**：
        # 软件光标已经让位（software-skip），于是光标位置那块后备缓冲必须是**纯背景**、
        # 不能有软件光标的黑白图案。若这里出现"白像素且左上相邻是黑"，说明软件光标还在画 = FAIL。
        if m_mv:
            cx, cy = int(m_mv.group(1)), int(m_mv.group(2))
            shot = vm.shot("cursor_on")
            if not shot:
                ck.ch("抓到屏幕帧", False)
            else:
                path, w, h, body = shot
                cursor_like = 0
                nonzero = 0

                def pxf(x, y):
                    o = (y * w + x) * 3
                    return body[o], body[o + 1], body[o + 2]

                for y in range(max(1, cy), min(h - 1, cy + 28)):
                    for x in range(max(1, cx), min(w - 1, cx + 28)):
                        r, g, b = pxf(x, y)
                        if r > 230 and g > 230 and b > 230:
                            l = pxf(x - 1, y)
                            u = pxf(x, y - 1)
                            if (max(l) < 40) or (max(u) < 40):
                                cursor_like += 1
                        if r < 40 and g < 40 and b < 40:
                            nonzero += 1
                ck.ch("光标位置 (x=%d,y=%d) 的后备缓冲里没有软件光标影子（黑边点 %d，黑像素 %d）"
                      % (cx, cy, cursor_like, nonzero),
                      cursor_like == 0 and nonzero == 0,
                      "cursor_like=%d dark=%d" % (cursor_like, nonzero))
                print("  [info] 帧 %dx%d（%s）；QEMU cursor 平面不进 screendump，这条断言的是'软件没画'"
                      % (w, h, os.path.basename(path)))
        for bad in ("PANIC", "TRIPLE FAULT"):
            ck.ch("正向：无 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass
    time.sleep(0.8)

    # ================= 反向：没有 virtio-gpu（-vga std）=================
    vm = V.Vm(qemu, IMG, "std", V.free_port(), WORK, "cursor=1")
    try:
        vm.wait_log("[VGPU] not found", 180)
        ck.ch("无设备时如实报告（[VGPU] not found）", "[VGPU] not found" in vm.log())
        ck.ch("光标回退软件路径（[VGPU] cursor disable reason=no-device）",
              "[VGPU] cursor disable reason=no-device" in vm.log())
        ok_desk = False
        if vm.wait_log(T.LOCK_INTERACTIVE, 240):
            vm.mon_cmd("sendkey ret")
            time.sleep(1.4)
            vm.mon_cmd("sendkey ret")
            ok_desk = vm.wait_log(T.DESKTOP_READY, 180)
        ck.ch("反向：系统照常进桌面（降级不影响可用性）", ok_desk)
        ck.ch("反向：软件光标照画（software-skip 一次都不出现）",
              "[VGPU] cursor software-skip" not in vm.log())
        for bad in ("PANIC", "TRIPLE FAULT"):
            ck.ch("反向：无 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    fails = ck.fails()
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if fails else "PASS", len(ck.items), len(ck.items) - len(fails)))
    for n, _, d in fails:
        print("  FAIL:", n, ("-- " + d) if d else "")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
