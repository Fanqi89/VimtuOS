"""tests/gif64_test.py - GIF（首帧静态）解码验收：内嵌字节 == 仓库里的真文件，像素对宿主真值

背景：到 v0.9.26 为止 `img64` 只有 PNG / JPEG / BMP；`settings64` 对 `.gif` **明确拒绝**
（`reason=gif not implemented`）。而用户自带的头像/壁纸里 GIF 很常见 —— 这是文件系统能力清单里
明确列出的一个缺口（"GIF 不支持"）。

本批（v0.9.27）：
  * `kernel/img64.cpp` 新增 `decode_gif64()`：GIF87a/89a、全局/局部色表、透明色索引、隔行、
    LZW 跨子块；**只解首帧**（动图取第一帧静态显示，不做动画/不做 disposal 合成）；
  * `img64_decode64` 与 `img64_decode_fit64` 都接上（fit 路径解码后按 max_side 收尾缩放）；
  * `settings64` 的头像与壁纸两条路径放开 GIF（原拒绝分支删掉，文案改成 "…/GIF（取首帧）"）。

判据（三层，互相独立）：
  ① `[IMG64] selftest gif ok=1 g1=4x4 g2=16x16 … mask=0`：内核里跑的两个**真 GIF** 解码结果；
  ② 内嵌字节 == 仓库里 `res/giftest/g1.gif` / `g2.gif` 的字节（正则从 img64.cpp 抠出数组逐字节比）；
  ③ 宿主 Pillow 解码同一份文件得到的像素 == 内核自检用的真值表（从 img64.cpp 抠 `kTestGif1Pix` 比）。
     —— 也就是说"内核解出来的像素"与"宿主编解码器解出来的像素"是同一套数，不是自己证明自己。

边界（如实）：只验证首帧；g2 是两帧文件，自检里明确写 `frames=1(first-only)`。

用法：py -3 tests\\gif64_test.py [--qemu 路径] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题
"""
import argparse
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

import proc64_test as p64          # noqa: E402  夹具 / QEMU 路径

IMG64_C = os.path.join(ROOT, "kernel", "img64.cpp")
GIF_DIR = os.path.join(ROOT, "res", "giftest")

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[IMG64] selftest FAIL",
    "[MEM64] OOM",
]


def q(path):
    return path.replace("\\", "/")


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_re(path, pattern, timeout, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, slog(path))
        if m:
            return m
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return None


def extract_c_array(name):
    """从 img64.cpp 里抠出 `static const uint8_t <name>[] = { 0x.., ... };` 的字节"""
    src = open(IMG64_C, "r", encoding="utf-8").read()
    m = re.search(r"static const uint8_t\s+" + name + r"\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def extract_pix():
    """从 img64.cpp 里抠出 kTestGif1Pix 的 16 个 0xRRGGBB"""
    src = open(IMG64_C, "r", encoding="utf-8").read()
    m = re.search(r"static const uint32_t\s+kTestGif1Pix\[16\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{6})", m.group(1))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    # ==================== 静态判据（不需要 QEMU）====================
    print("=== A) 内嵌字节 vs 仓库里的真 GIF；真值像素 vs 宿主 Pillow ===")
    for name, fn in (("kTestGif1", "g1.gif"), ("kTestGif2", "g2.gif")):
        p = os.path.join(GIF_DIR, fn)
        if not os.path.exists(p):
            print("缺少 %s（先跑 tools/ 或 res/giftest 生成）" % p)
            return 2
        disk = open(p, "rb").read()
        emb = extract_c_array(name)
        check("img64.cpp 内嵌 %s == res/giftest/%s 字节" % (name, fn),
              emb is not None and emb == disk,
              "嵌入 %s B / 文件 %s B" % (len(emb) if emb else "?", len(disk)))
    try:
        from PIL import Image
        truth = []
        im = Image.open(os.path.join(GIF_DIR, "g1.gif")).convert("RGB")
        for y in range(4):
            for x in range(4):
                r, g, b = im.getpixel((x, y))
                truth.append((r << 16) | (g << 8) | b)
        got = extract_pix()
        check("kTestGif1Pix == 宿主 Pillow 读 g1.gif 的像素", got == truth,
              "%s" % ("一致" if got == truth else "内核表=%s 宿主=%s" % (got, truth)))
        im2 = Image.open(os.path.join(GIF_DIR, "g2.gif"))
        check("g2.gif 是多帧（用来说明自检只看首帧）",
              getattr(im2, "n_frames", 1) >= 2,
              "n_frames=%s size=%s" % (getattr(im2, "n_frames", 1), im2.size))
    except ImportError:
        print("  （没有 Pillow：跳过宿主真值比对，只做字节比对）")

    # ==================== 客人侧（真 QEMU）====================
    print("=== B) 真 QEMU：内核自检里的两个 GIF ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_gif_")
    disk = os.path.join(tmp, "gif.img")
    shutil.copyfile(base, disk)                 # ★ 副本：绝不直接引导共享夹具
    serial = os.path.join(tmp, "serial.log")
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-gif", "-drive", "format=raw,file=%s" % q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_re(serial, r"\[IMG64\] selftest gif ok=(\d) g1=(\d+)x(\d+) g2=(\d+)x(\d+) "
                            r"frames=1\(first-only\) transp=(\d+) mask=(\d+)", 240, proc)
        check("[IMG64] selftest gif 行出现且 ok=1 / mask=0",
              m is not None and m.group(1) == "1" and m.group(7) == "0",
              m.group(0) if m else "（没等到 selftest gif 行）")
        if m:
            check("g1 解出 4x4", m.group(2) == "4" and m.group(3) == "4",
                  "%sx%s" % (m.group(2), m.group(3)))
            check("g2 解出 16x16", m.group(4) == "16" and m.group(5) == "16",
                  "%sx%s" % (m.group(4), m.group(5)))
            check("透明色索引被识别（g2 transparency=0）", m.group(6) == "0",
                  "transp=%s" % m.group(6))
        check("[IMG64] selftest PASS（总自检没被 GIF 段拉红）",
              wait_re(serial, r"\[IMG64\] selftest PASS mask=0", 120, proc) is not None, "")
        txt = slog(serial)
        bad = [w for w in FORBIDDEN if w in txt]
        check("没有禁止行", not bad, "、".join(bad) if bad else "")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=20)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        if args.keep:
            print("日志保留：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    passed = sum(1 for _n, c in checks if c)
    print("=== %d/%d PASS ===" % (passed, len(checks)))
    return 0 if passed == len(checks) and checks else 1


if __name__ == "__main__":
    sys.exit(main())
