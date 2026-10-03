#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/jpeg64_test.py - kernel/img64.cpp 的**基线 JPEG 解码**端到端验收（与宿主 Pillow 逐像素对照）

被验的需求：img64 原先对 `FF D8` 直接打 "jpeg not implemented in this batch" 返回 -2；本批要
**补齐基线 JPEG（SOF0/SOF1）**：Huffman 解码、DC/AC 系数、反量化、IDCT、YCbCr->RGB、
4:4:4/4:2:2/4:2:0 色度采样、RSTn 重启标记、EXIF Orientation（至少 1/3/6/8）；渐进式（SOF2）
允许如实不支持（明确错误码 + 打点，别崩）。

做法（全链路真跑，不是模拟）：
  1) tools/jpeg_make_testset.py 用 Pillow 生成 12 张测试图（纯色/渐变/文字/噪声 + 三种色度采样
     各一 + 3 张带 EXIF 旋转 + 灰度 + 截断/渐进式/损坏三种错误路径）；
  2) 本脚本用 tools/make_shellvol.py 的 Volume 把图集**写进 VimtuFS2 主分区卷**（+一个空标记文件
     /jpegtest/px.on），再拼成一块可启动的夹具盘（system.img + 标准 MBR + 主分区卷）；
  3) QEMU 引导：内核启动期（gui64 初始化里）会扫 /jpegtest/jNN.jpg 逐个解码，打点
       [JPEG64] selftest file=… bytes=… rc=… fmt=jpeg w=… h=… orient=… comps=… samp=… rst=… fnv=… err=…
     并在有 px.on 时把**解码像素逐行 dump** 到串口：
       [JPEG64] px jNN.jpg row=<y> w=<w> data=<16 进制 RRGGBB…>
  4) 本脚本把 dump 还原成像素阵列，与 Pillow 解同一张图的结果对照：
       * 平均绝对误差 MAE、最大逐像素差 max ；
       * **结构一致**判据：两边各缩到 16x16 块（16x16 区域平均）后逐块比较的均方误差
         （阈值见下面的 MAE_MAX / PXDIFF_MAX / BLOCK_MSE_MAX 与注释里的依据）；
       * EXIF 图额外断言：**旋转后的尺寸是转置的**，且与 Pillow exif_transpose 后的像素走向一致；
  5) 错误路径：截断 -> rc=3；渐进式 -> rc=2（err 里写明 "progressive jpeg (SOF2) unsupported"）；
     损坏 -> rc∈{2,3}；全程不得 PANIC / TRIPLE FAULT / selftest FAIL。

★ 阈值依据（JPEG 是有损格式，"与 Pillow 不一致"本身不是缺陷；要钉的是"结构一致 + 没有系统性偏差"）：
  * Pillow 走 libjpeg-turbo：isdct 快速整数 IDCT + 2x "fancy upsampling"；内核走自己的整数 IDCT
    （与精确 IDCT 对照实测最大 1 LSB、平均 0.08 LSB —— 见 kernel/img64.cpp 的说明）+ 同款 fancy 权重。
    两者在**高频边缘**（文字/噪声）必然有 LSB 级差异；
  * 因此逐像素 MAE 阈值按"实测值 × 约 2 倍余量"钉，块 MSE 阈值按"实测值 × 约 3 倍余量"钉
    （把实测数字写进 --verbose 输出，脚本每次跑都会重新打印，便于复核）。

用法：py -3 tests\\jpeg64_test.py [--qemu 路径] [--timeout 300] [--keep] [--verbose]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物/Pillow 缺失）
"""
import argparse
import importlib.util
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import qemuhelp as qh              # noqa: E402  （公共 QEMU 路径表/工具）
import proc64_test as p64          # noqa: E402  （复用它的 QEMU 候选路径表 find_qemu）


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


MSV = _load("make_shellvol", os.path.join(ROOT, "tools", "make_shellvol.py"))
GEN = _load("jpeg_make_testset", os.path.join(ROOT, "tools", "jpeg_make_testset.py"))

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "jpeg64_test.img")
TARGET_SECTORS = 32768
IGNORE = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL"]

# ---- 对照阈值（口径见文件头）----
MAE_MAX = 5.0            # 全图逐像素平均绝对误差（含噪声图，实测值见输出）
PXDIFF_MAX = 110         # 单像素最大差（只出现在文字/噪声的高频边缘）
BLOCK_MSE_MAX = 220.0    # 16x16 块均值 MSE（结构一致判据）
SOLID_MAE_MAX = 2.0      # 纯色图要求更严（几乎没有高频）
SOLID_BLOCK_MSE_MAX = 6.0


def q(p):
    return p.replace("\\", "/")




def build_fixture(files, out_img):
    """把 /jpegtest/* 写进 VimtuFS2 主分区卷，再拼一块可启动夹具盘。"""
    if not os.path.exists(SYSTEM_IMG):
        return None, "缺少构建产物 %s（先跑 bash build64.sh）" % SYSTEM_IMG
    sys_bytes = open(SYSTEM_IMG, "rb").read()
    if len(sys_bytes) == 0 or len(sys_bytes) % MSV.SECTOR:
        return None, "%s 不是扇区对齐的内核镜像" % SYSTEM_IMG
    if len(sys_bytes) > TARGET_SECTORS * MSV.SECTOR:
        return None, "%s 比目标盘还大" % SYSTEM_IMG
    vol = MSV.Volume(TARGET_SECTORS - MSV.PART_MAIN_LBA)
    d = vol.mkdir("jpegtest", parent=0, mode=0o755)
    expect = {}
    for name in sorted(files):
        vol.write_file(name, files[name], parent=d, mode=0o644)
        expect["/jpegtest/" + name] = files[name]
    vol.write_file("px.on", b"", parent=d, mode=0o644)
    expect["/jpegtest/px.on"] = b""
    vol_bytes = vol.finish()
    bad = MSV.verify(vol_bytes, expect)          # 独立回读：确认装进卷的字节逐字节一致
    if bad:
        return None, "卷自检失败：%s" % bad
    disk = MSV.build_disk(sys_bytes, vol_bytes, TARGET_SECTORS)
    with open(out_img, "wb") as f:
        f.write(disk)
    return out_img, None


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def find_qemu(explicit=None):
    return p64.find_qemu(explicit)


def wait_for(path, needle, timeout, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = slog(path)
        if needle in s:
            return s
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.5)
    return slog(path)


def block_image(arr, bs=16):
    """把 HxW x 3 的图像按 bs x bs 区域平均成 (H//bs)x(W//bs) x 3 的块均值（余数裁掉）。"""
    h, w = arr.shape[0], arr.shape[1]
    nh, nw = h // bs, w // bs
    out = []
    for by in range(nh):
        for bx in range(nw):
            blk = arr[by * bs:(by + 1) * bs, bx * bs:(bx + 1) * bs, :]
            out.append(blk.reshape(-1, 3).mean(axis=0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    try:
        import numpy as np
        from PIL import Image, ImageOps
    except Exception as exc:
        sys.stderr.write("需要 Pillow + numpy：%s\n" % exc)
        return 2

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    files, meta = GEN.build_testset()
    if not files:
        sys.stderr.write("生成测试图集失败\n")
        return 2

    img, err = build_fixture(files, FIXTURE_IMG)
    if not img:
        sys.stderr.write("%s\n" % err)
        return 2
    print("[jpeg64] 夹具盘：%s（%d 张测试图已装进主分区卷 /jpegtest/）"
          % (img, len(files)))

    tmp = tempfile.mkdtemp(prefix="vimtu64_jpeg_")
    serial = os.path.join(tmp, "jpeg.log")
    args_q = [qemu, "-name", "Vimtu64-jpeg64",
              "-drive", "format=raw,file=%s" % q(img),
              "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
              "-serial", "file:%s" % q(serial), "-no-reboot"]
    proc = subprocess.Popen(args_q, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    try:
        log = wait_for(serial, "[JPEG64] selftest files=", args.timeout, proc)
        if "[JPEG64] selftest files=" not in log:
            log = wait_for(serial, "[JPEG64] selftest skip", 20, proc)

        # ---- 0) 自检有没有真的跑起来 ----
        check("卷内 JPEG 自检跑起来了（[JPEG64] selftest files=）",
              "[JPEG64] selftest files=" in log,
              next((l for l in log.splitlines() if "[JPEG64] selftest" in l), "")[:160])
        check("内核到达桌面初始化（[GUI64] ready 或 img64 selftest PASS）",
              ("[GUI64] ready" in log) or ("[IMG64] selftest PASS" in log))
        # 内核侧汇总：12 槽位齐全时必须是 9 张成功 + 3 张明确错误码（错误路径也按设计走通了）；
        # 且自检**不得**因此报红（img64_selftest64 的 mask 里不许出现 128 位）。
        ms = re.search(r"\[JPEG64\] selftest files=(\d+) ok0=(\d+) err2_3=(\d+) other=(\d+) "
                       r"dump=(\d+) ok=(\d+) rc=(\d+)", log)
        check("内核汇总 [JPEG64] selftest files=.. ok0=9 err2_3=3 other=0 ok=1 rc=0",
              bool(ms) and ms.group(1) == "12" and ms.group(2) == "9" and ms.group(3) == "3" and
              ms.group(4) == "0" and ms.group(5) == "1" and ms.group(6) == "1" and ms.group(7) == "0",
              ms.group(0) if ms else "（缺汇总行）")
        check("img64 自检未因 JPEG 错误路径用例报红（无 [IMG64] selftest mask=128）",
              "mask=128" not in log)

        rows = {}
        row_re = re.compile(
            r"\[JPEG64\] selftest file=(\S+) bytes=(\d+) rc=(-?\d+) fmt=(\S+) w=(\d+) h=(\d+) "
            r"orient=(\d+) comps=(\d+) samp=(\S+) rst=(\d+) fnv=([0-9A-Fa-f]{16}) err=(.*)$",
            re.M)
        for m in row_re.finditer(log):
            rows[m.group(1)] = dict(bytes=int(m.group(2)), rc=int(m.group(3)), fmt=m.group(4),
                                    w=int(m.group(5)), h=int(m.group(6)), orient=int(m.group(7)),
                                    comps=int(m.group(8)), samp=m.group(9), rst=int(m.group(10)),
                                    fnv=m.group(11), err=m.group(12).strip())
        check("12 个槽位的 [JPEG64] selftest file= 行都在", len(rows) == len(meta),
              "rows=%d 期望=%d" % (len(rows), len(meta)))

        # ---- 1) 每个文件的 rc / 尺寸 / 特征 ----
        for name, mt in sorted(meta.items()):
            r = rows.get(name)
            if r is None:
                check("%s 有解码打点" % name, False)
                continue
            check("%s rc=%d（期望 %d）" % (name, mt["expect_rc"], mt["expect_rc"]),
                  r["rc"] == mt["expect_rc"] or (mt["expect_rc"] == 2 and r["rc"] in (2, 3)),
                  "rc=%d err=%s" % (r["rc"], r["err"]))
            if mt["expect_rc"] == 0:
                o = mt["orient"]
                ew, eh = (mt["h"], mt["w"]) if o >= 5 else (mt["w"], mt["h"])
                check("%s 解码尺寸 %dx%d（EXIF orient=%d 后应为 %dx%d）"
                      % (name, r["w"], r["h"], o, ew, eh),
                      r["w"] == ew and r["h"] == eh,
                      "内核 %dx%d" % (r["w"], r["h"]))
                check("%s orient 打点 = %d" % (name, o), r["orient"] == o,
                      "内核 orient=%d" % r["orient"])
                if mt["samp"] != "-":
                    check("%s 色度采样 = %s" % (name, mt["samp"]), r["samp"] == mt["samp"],
                          "内核 samp=%s" % r["samp"])
                check("%s 分量数 = %s" % (name, "1" if mt["samp"] == "gray" else "3"),
                      r["comps"] == (1 if mt["samp"] == "gray" else 3),
                      "内核 comps=%d" % r["comps"])
                if mt["want_rst"]:
                    check("%s 解到了 RSTn 重启标记（rst>0）" % name, r["rst"] > 0,
                          "内核 rst=%d" % r["rst"])
        check("渐进式（j09）错误码是 2 且写明 progressive/SOF2",
              rows.get("j09.jpg", {}).get("rc") == 2 and
              "progressive" in rows.get("j09.jpg", {}).get("err", "").lower(),
              rows.get("j09.jpg", {}).get("err", ""))
        check("截断（j08）错误码是 3（数据损坏，不是'不支持'）",
              rows.get("j08.jpg", {}).get("rc") == 3, rows.get("j08.jpg", {}).get("err", ""))

        # ---- 2) 像素 dump 与 Pillow 对照 ----
        px = {}
        px_re = re.compile(r"\[JPEG64\] px (\S+) row=(\d+) w=(\d+) data=([0-9a-f]+)$", re.M)
        for m in px_re.finditer(log):
            name, y, w, hexs = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            if len(hexs) != w * 6:
                continue
            row = np.zeros((w, 3), dtype=np.uint16)
            for i in range(w):
                h6 = hexs[i * 6:(i + 1) * 6]
                row[i] = (int(h6[0:2], 16), int(h6[2:4], 16), int(h6[4:6], 16))
            px.setdefault(name, {})[y] = row
        got_full = {}
        for name, rs in px.items():
            if not rs:
                continue
            hmax = max(rs)
            if len(rs) == hmax + 1:
                got_full[name] = np.stack([rs[y] for y in range(hmax + 1)], axis=0)

        check("像素 dump 覆盖全部成功解码的图（%d 张）" % len(got_full), len(got_full) >= 6,
              "dump=%s" % sorted(got_full))

        stats = []
        for name, mt in sorted(meta.items()):
            if mt["expect_rc"] != 0:
                continue
            kern = got_full.get(name)
            if kern is None:
                check("%s 有完整像素 dump" % name, False)
                continue
            import io
            im = Image.open(io.BytesIO(files[name]))
            ref = np.asarray(ImageOps.exif_transpose(im).convert("RGB"), dtype=np.int32)
            kk = kern.astype(np.int32)
            if ref.shape != kk.shape:
                check("%s 与 Pillow 尺寸一致（%s）" % (name, ref.shape), False,
                      "内核 %s" % (kk.shape,))
                continue
            diff = np.abs(ref - kk)
            mae = float(diff.mean())
            mx = int(diff.max())
            kb = block_image(ref.astype(np.float64))
            gb = block_image(kk.astype(np.float64))
            bmse = float(np.mean([((a - b) ** 2).mean() for a, b in zip(kb, gb)]))
            exact = float((diff == 0).mean() * 100.0)
            stats.append((name, mae, mx, bmse, exact, ref.shape))
            solid = (name == "j01.jpg")
            check("%s 结构一致：MAE=%.3f ≤ %.1f（块 MSE=%.2f ≤ %.1f，最大差 %d）"
                  % (name, mae, SOLID_MAE_MAX if solid else MAE_MAX, bmse,
                     SOLID_BLOCK_MSE_MAX if solid else BLOCK_MSE_MAX, mx),
                  mae <= (SOLID_MAE_MAX if solid else MAE_MAX) and
                  bmse <= (SOLID_BLOCK_MSE_MAX if solid else BLOCK_MSE_MAX),
                  "逐像素相同 %.1f%%" % exact)
            check("%s 单像素最大差 %d ≤ %d" % (name, mx, PXDIFF_MAX), mx <= PXDIFF_MAX)

        if args.verbose or stats:
            print("  --- 与 Pillow 的对照数字（MAE / 最大差 / 16x16 块 MSE / 完全相同比例） ---")
            for name, mae, mx, bmse, exact, shape in stats:
                print("      %-9s %sx%s  MAE=%.3f  max=%3d  blockMSE=%8.2f  exact=%5.1f%%"
                      % (name, shape[0], shape[1], mae, mx, bmse, exact))

        # ---- 3) 禁止出现 ----
        for needle in IGNORE:
            check("不得出现 %s" % needle, needle not in log)

        print("--- serial tail ---")
        for line in [x for x in log.splitlines() if x.strip()][-10:]:
            print("   | " + line[:170])
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    if args.keep:
        print("[jpeg64] 串口日志：%s" % serial)
    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
