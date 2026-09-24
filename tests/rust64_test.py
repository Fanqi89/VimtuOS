#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/rust64_test.py - "Rust 正式接入 VimtuOS 内核"验收（静态链接证据 + QEMU 串口证据）

验收对象（gui_rs crate = 设计 Token 表 + 主题配色计算，只链进**系统内核**）：
  1) 源码真源：gui_rs/src/tokens.rs 的关键 Token 值 == 视觉线的设计约定
     （圆角 14/12/9/24/24/10、模糊 24/12、透明 450/800/1000、阴影 0.08/0.12、动效 150/225/330、
      缓动 200/0/0/1000），并且这些**常量**确实被 Token 表引用（防止"常量改了表没改"）。
  2) 主题表：gui_rs/src/theme.rs 里 6 个主题（白色(默认)/暗色/蓝白/粉白/粉绿/粉紫）解析成
     "名字 + accent 十六进制"；**accent 的期望值从 Rust 源码解析**，不写死第二份。
  3) 链接证据（nm/objdump）：
     * gui_rs/gui_rs.o 里 7 个规定导出符号 + 扩展符号全部是已定义（T）；
     * 最终**系统内核** build64/kernel64_os.elf 里这些符号也都在（T）；
     * 扁平镜像 build64/kernel64_os.bin / build64/system.img 里能按字节搜到 Rust 的
       BUILD_TAG（证明 .rodata 真的进了最终镜像，不是只编译出来没链）；
     * **安装介质内核** build64/kernel64.bin / kernel64.elf 里既没有这些符号、也没有该标记
       （安装内核不链 Rust 目标文件）；
     * gui_rs.o 里没有任何 xmm/SSE 指令（引导链没有开 CR4.OSFXSR），且未定义符号只剩
       memcpy（内核 mem64.cpp 提供）。
  4) QEMU 启动系统镜像读串口：`[RUST64] tokens ok themes=6 accent=#RRGGBB selftest PASS`
     行存在、themes/accent 与源码解析值一致、且启动日志里没有 Rust panic / 蓝屏。
  5) 体积：系统内核与安装内核都 < 4,096,000 B（内核区硬上限）。

用法：
    py -3 tests/rust64_test.py
    py -3 tests/rust64_test.py --keep-serial /tmp/rust64_serial.log
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题（工具/QEMU/镜像缺失）
"""
import argparse
import os
import re
import shutil
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
# nm / objdump：优先 MSYS2 的（binutils 或 LLVM）。任务规定用 MSYS2 的 llvm-nm 或 nm。
NM_CANDIDATES = [
    r"C:\msys64\usr\bin\llvm-nm.exe", r"C:\msys64\mingw64\bin\llvm-nm.exe",
    r"C:\msys64\usr\bin\nm.exe", r"C:\msys64\mingw64\bin\nm.exe",
    "llvm-nm", "nm",
]
OBJDUMP_CANDIDATES = [
    r"C:\msys64\usr\bin\llvm-objdump.exe", r"C:\msys64\mingw64\bin\llvm-objdump.exe",
    r"C:\msys64\usr\bin\objdump.exe", r"C:\msys64\mingw64\bin\objdump.exe",
    "llvm-objdump", "objdump",
]

# ---- 设计要求（与任务书/视觉线约定一致；这是"规格"，不是"实现"的副本）----
SPEC_THEME_NAMES = ["白色(默认)", "暗色", "蓝白渐变", "粉白渐变", "粉绿渐变", "粉紫渐变"]
SPEC_TOKEN_PX = {                       # i32 常量 -> 期望值
    "RADIUS_WINDOW_PX": 14, "RADIUS_CARD_PX": 12, "RADIUS_BUTTON_PX": 9,
    "RADIUS_DOCK_PX": 24, "RADIUS_START_TOP_PX": 24, "RADIUS_START_BOTTOM_PX": 10,
    "BLUR_BACKGROUND_PX": 24, "BLUR_CONTENT_PX": 12,
    "SHADOW_NEAR_DY_PX": 2, "SHADOW_NEAR_BLUR_PX": 4,
    "SHADOW_FAR_DY_PX": 12, "SHADOW_FAR_BLUR_PX": 32,
    "SHADOW_DARK_NEAR_BLUR_PX": 6, "SHADOW_DARK_FAR_BLUR_PX": 36,
    "MOTION_FAST_MS": 150, "MOTION_NORMAL_MS": 225, "MOTION_PANEL_MS": 330,
    "FONT_BODY_PX": 14,
}
SPEC_TOKEN_PERMILLE = {                 # u32 千分比常量 -> 期望值
    "ALPHA_BACKDROP_MATERIAL": 450, "ALPHA_CONTENT_CARD": 800, "ALPHA_TEXT": 1000,
    "SHADOW_NEAR_ALPHA": 80, "SHADOW_FAR_ALPHA": 120,
    "SHADOW_DARK_NEAR_ALPHA": 180, "SHADOW_DARK_FAR_ALPHA": 280,
    "EASE_X1": 200, "EASE_Y1": 0, "EASE_X2": 0, "EASE_Y2": 1000,
}
REQUIRED_SYMS = [
    "rust64_tokens_init64", "rust64_theme_count64", "rust64_theme_name64",
    "rust64_theme_colors64", "rust64_theme_set64", "rust64_theme_current64",
    "rust64_accent_rgb64",
]
EXTRA_SYMS = [
    "rust64_token_count64", "rust64_token_lookup64", "rust64_token_name64",
    "rust64_token_px64", "rust64_token_permille64", "rust64_theme_shadow64",
    "rust64_theme_gradient64", "rust64_accent_on_rgb64", "rust64_blend_rgb64",
    "rust64_build_tag64", "rust64_build_tag_len64",
    "rust64_panic_hook_set64", "rust64_panicked64",
]
KERNEL_MAX_BYTES = 4096000

# QEMU 串口行（kernel64.cpp 打点，格式必须与源码一致）
RUST64_RX = re.compile(
    r"\[RUST64\] tokens ok themes=(\d+) accent=#([0-9A-Fa-f]{6}) selftest (PASS|FAIL)")


class Checks(object):
    """断言收集器：每条都打一行 PASS/FAIL，最后统计条数。"""

    def __init__(self):
        self.n = 0
        self.fail = 0

    def ok(self, name, cond, detail=""):
        self.n += 1
        good = bool(cond)
        if not good:
            self.fail += 1
        line = "  [%s] %s" % ("PASS" if good else "FAIL", name)
        if detail:
            line += "  (%s)" % detail
        print(line)
        return good

    def report(self):
        print("--- RUST64 验收：%d 条断言，%d 条失败 ---" % (self.n, self.fail))
        return self.fail == 0


def find_tool(cands, what):
    for c in cands:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    sys.stderr.write("找不到 %s（试过：%s）\n" % (what, ", ".join(cands)))
    return None


def sh(args, cwd=ROOT):
    """跑一条命令并回 (rc, stdout+stderr)。"""
    try:
        p = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=300)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except Exception as e:                                  # pragma: no cover
        return 127, "spawn failed: %r" % (e,)


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


class Mon(object):
    """QEMU monitor（telnet）客户端：只用来注入键盘（sendkey）。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def key(self, name, wait=0.35):
        for _ in range(3):
            if self.send("sendkey %s" % name, wait=wait):
                return True
            time.sleep(0.2)
        return False

    def type_line(self, text, per_key=0.16):
        """把一行 ASCII 敲进当前窗口（只支持本测试用到的字符集）。"""
        names = {" ": "spc", ".": "dot", "-": "minus"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char for sendkey: %r" % ch)
        self.key("ret", wait=per_key + 0.2)


def pick_port(start=5600, end=5699):
    """挑一个空闲 TCP 端口给 QEMU monitor 用（被占用就换下一个）。"""
    for p in range(start, end + 1):
        try:
            s = socket.socket()
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            continue
    return None


def rgba_values(expr):
    """把 `0x00, 0x54, 0x9E` / `255, 255, 255, 204` 之类解析成 int 列表。"""
    out = []
    for part in expr.split(","):
        part = part.strip()
        if not part:
            continue
        out.append(int(part, 0))
    return out


def parse_themes(src):
    """解析 gui_rs/src/theme.rs 的 THEMES 表 -> [dict(..., name, accent_rgb, ...)]。"""
    m = re.search(r"pub static THEMES: \[ThemeDef; (\d+)\] = \[(.*?)\n\];", src, re.S)
    if not m:
        raise ValueError("theme.rs 里没找到 THEMES 表（正则失配）")
    declared, body = int(m.group(1)), m.group(2)
    themes = []
    for block in body.split("ThemeDef {")[1:]:
        def grab(field):
            mm = re.search(field + r":\s*([^,\n]+)", block)
            return mm.group(1).strip() if mm else None
        name = grab("name")
        acc = re.search(r"accent:\s*Rgba::(?:rgb|new)\(([^)]*)\)", block)
        gs = re.search(r"grad_start:\s*Rgba::(?:rgb|new)\(([^)]*)\)", block)
        ge = re.search(r"grad_end:\s*Rgba::(?:rgb|new)\(([^)]*)\)", block)
        if not (name and acc and gs and ge):
            raise ValueError("theme 块解析失败：%r" % block[:80])
        a = rgba_values(acc.group(1))
        themes.append({
            "name": name.strip('"'),
            "accent_rgb": (a[0] << 16) | (a[1] << 8) | a[2],
            "dark": grab("dark") == "true",
            "dock": grab("dock"),
            "grad_start": rgba_values(gs.group(1))[:3],
            "grad_end": rgba_values(ge.group(1))[:3],
        })
    return declared, themes


def parse_build_tag(src):
    m = re.search(r'pub const BUILD_TAG: &\[u8\] = b"([^"]+)";', src)
    if not m:
        raise ValueError("lib.rs 里没找到 BUILD_TAG（验收脚本靠它做字节级链接证据）")
    return m.group(1).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"),
                    help="已安装系统镜像（默认 build64/system.img = 系统内核）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150, help="等 [RUST64] 行的秒数")
    ap.add_argument("--keep-serial", default=None, help="把串口日志另存一份（取证用）")
    ap.add_argument("--skip-qemu", action="store_true", help="只跑静态（nm/解析/体积）部分")
    args = ap.parse_args()

    ck = Checks()
    print("=== Rust 接入验收（gui_rs: 设计 Token + 主题配色）===")

    # ---------- 0) 文件与工具 ----------
    f_tokens = os.path.join(ROOT, "gui_rs", "src", "tokens.rs")
    f_theme = os.path.join(ROOT, "gui_rs", "src", "theme.rs")
    f_lib = os.path.join(ROOT, "gui_rs", "src", "lib.rs")
    f_rs_obj = os.path.join(ROOT, "gui_rs", "gui_rs.o")
    f_os_elf = os.path.join(ROOT, "build64", "kernel64_os.elf")
    f_os_bin = os.path.join(ROOT, "build64", "kernel64_os.bin")
    f_ins_bin = os.path.join(ROOT, "build64", "kernel64.bin")
    f_ins_elf = os.path.join(ROOT, "build64", "kernel64.elf")
    f_sysimg = os.path.join(ROOT, "build64", "system.img")
    nm = find_tool(NM_CANDIDATES, "nm/llvm-nm")
    objdump = find_tool(OBJDUMP_CANDIDATES, "objdump/llvm-objdump")
    if not nm or not objdump:
        return 2
    for p in (f_tokens, f_theme, f_lib, f_rs_obj, f_os_elf, f_os_bin, f_ins_bin, f_ins_elf, f_sysimg):
        if not os.path.exists(p):
            sys.stderr.write("缺文件：%s（先跑 bash build64.sh）\n" % p)
            return 2
    print("[env] nm=%s\n      objdump=%s" % (nm, objdump))

    src_tokens, src_theme, src_lib = read_text(f_tokens), read_text(f_theme), read_text(f_lib)

    # ---------- 1) Token 表 == 设计约定 ----------
    print("=== 1) 设计 Token（唯一真源 gui_rs/src/tokens.rs）===")
    table_body = ""
    mt = re.search(r"pub static TOKENS: \[TokenDef; (\d+)\] = \[(.*?)\n\];", src_tokens, re.S)
    token_count = int(mt.group(1)) if mt else -1
    if mt:
        table_body = mt.group(2)
    ck.ok("tokens.rs 有 TOKENS 表（声明 %d 条）" % token_count, bool(table_body) and token_count > 0)
    for cname, want in sorted(SPEC_TOKEN_PX.items()):
        m = re.search(r"pub const %s: i32 = (-?\d+);" % cname, src_tokens)
        got = int(m.group(1)) if m else None
        ck.ok("Token %s == %d" % (cname, want), got == want, "源码=%s" % got)
        ref = ("value: %s," % cname) in table_body
        ck.ok("Token 表引用 %s（不是各写一份）" % cname, ref)
    for cname, want in sorted(SPEC_TOKEN_PERMILLE.items()):
        m = re.search(r"pub const %s: u32 = (\d+);" % cname, src_tokens)
        got = int(m.group(1)) if m else None
        ck.ok("Token %s == %d%%" % (cname, want), got == want, "源码=%s" % got)
        ref = ("permille: %s }" % cname) in table_body or ("permille: %s," % cname) in table_body
        ck.ok("Token 表引用 %s" % cname, ref)

    # ---------- 2) 主题表（accent 期望值从源码解析）----------
    print("=== 2) 主题表（gui_rs/src/theme.rs）===")
    declared, themes = parse_themes(src_theme)
    names = [t["name"] for t in themes]
    ck.ok("THEMES 声明 %d 个主题" % declared, declared == 6 and len(themes) == 6,
          "解析到 %d" % len(themes))
    ck.ok("主题名与约定一致（顺序固定）", names == SPEC_THEME_NAMES, "解析=%s" % names)
    ck.ok("默认主题 = 白色(默认) 且非暗色", themes[0]["name"] == "白色(默认)" and not themes[0]["dark"])
    dark_idx = [i for i, t in enumerate(themes) if t["dark"]]
    ck.ok("恰好 1 个暗色主题（索引 1）", dark_idx == [1], "dark=%s" % dark_idx)
    ck.ok("非暗色主题 Dock 用固定默认色（同一常量）",
          all(t["dock"] == "DEFAULT_DOCK" for t in themes if not t["dark"]))
    ck.ok("暗色主题 Dock = DARK_DOCK（深灰半透）", themes[1]["dock"] == "DARK_DOCK")
    grads = [t for t in themes if t["grad_start"] != t["grad_end"]]
    ck.ok("4 个自定义渐变主题（蓝白/粉白/粉绿/粉紫）", len(grads) == 4,
          "渐变=%s" % [t["name"] for t in grads])
    tag = parse_build_tag(src_lib)
    print("[parse] 源码解析：theme0 accent=#%06X tag=%s" % (themes[0]["accent_rgb"], tag.decode()))

    # ---------- 3) 链接证据（nm + 字节搜索 + objdump）----------
    print("=== 3) 链接证据（符号 / 字节 / 指令集）===")
    rc, nm_obj = sh([nm, f_rs_obj])
    rc2, nm_os = sh([nm, f_os_elf])
    rc3, nm_ins = sh([nm, f_ins_elf])
    ck.ok("nm 能读 gui_rs/gui_rs.o", rc == 0, "rc=%d" % rc)

    def defined_syms(nm_out):
        s = set()
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-2].upper() in ("T", "W") and parts[-1].startswith("rust64_"):
                s.add(parts[-1])
        return s

    syms_obj, syms_os = defined_syms(nm_obj), defined_syms(nm_os)
    for s in REQUIRED_SYMS:
        ck.ok("gui_rs.o 定义 %s" % s, s in syms_obj)
    for s in EXTRA_SYMS:
        ck.ok("gui_rs.o 定义(扩展) %s" % s, s in syms_obj)
    for s in REQUIRED_SYMS:
        ck.ok("系统内核 kernel64_os.elf 里 %s 已链接" % s, s in syms_os)
    ck.ok("系统内核里有 Rust 符号（计数 >= %d）" % len(REQUIRED_SYMS),
          len(syms_os) >= len(REQUIRED_SYMS), "count=%d" % len(syms_os))

    # 未定义符号：只允许 memcpy（mem64.cpp 提供）；不允许 core 的 panic 符号
    undef = set()
    rc_u, nm_u = sh([nm, "-u", f_rs_obj])
    for line in nm_u.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2].upper() == "U":
            undef.add(parts[-1])
    ck.ok("gui_rs.o 未定义符号只有 memcpy", undef == {"memcpy"}, "未定义=%s" % sorted(undef))
    ck.ok("gui_rs.o 没有 core::panicking 未定义引用",
          not any("panicking" in u or "core" in u for u in undef))

    # SSE 检查：引导链没开 CR4.OSFXSR，Rust 代码不能出现 xmm
    rc_d, dis = sh([objdump, "-d", f_rs_obj])
    sse_hits = len(re.findall(r"\b(xmm\d+|movups|movaps|movdqa|movdqu|movss|addss|mulss)\b", dis))
    ck.ok("gui_rs.o 无 xmm/SSE 指令", rc_d == 0 and sse_hits == 0, "命中=%d" % sse_hits)

    # 字节级：Rust 的 .rodata 标记进了最终系统镜像；安装内核里没有
    bin_os = read_bytes(f_os_bin)
    bin_ins = read_bytes(f_ins_bin)
    img = read_bytes(f_sysimg)
    ck.ok("系统内核 kernel64_os.bin 含 Rust BUILD_TAG 字节", tag in bin_os)
    ck.ok("system.img（装好的系统盘）含 Rust BUILD_TAG 字节", tag in img)
    ck.ok("安装内核 kernel64.bin **不含** Rust BUILD_TAG 字节", tag not in bin_ins)
    ck.ok("安装内核 kernel64.elf **没有** Rust 符号",
          "rust64_" not in nm_ins, "nm 命中=%d" % nm_ins.count("rust64_"))
    ck.ok("安装内核 kernel64.elf nm 可读", rc3 == 0)

    # ---------- 4) 体积 ----------
    print("=== 4) 体积 ===")
    sz_os, sz_ins, sz_obj = len(bin_os), len(bin_ins), os.path.getsize(f_rs_obj)
    ck.ok("系统内核 %d B < %d B（硬上限）" % (sz_os, KERNEL_MAX_BYTES), sz_os < KERNEL_MAX_BYTES)
    ck.ok("安装内核 %d B < %d B" % (sz_ins, KERNEL_MAX_BYTES), sz_ins < KERNEL_MAX_BYTES)
    ck.ok("Rust 目标文件 gui_rs.o = %d B（非空，且远小于内核上限）" % sz_obj,
          0 < sz_obj < 262144)
    print("[size] kernel64_os.bin=%d B  kernel64.bin=%d B  gui_rs.o=%d B" % (sz_os, sz_ins, sz_obj))

    # ---------- 5) QEMU 串口证据 ----------
    serial = ""
    if not args.skip_qemu:
        print("=== 5) QEMU 启动 system.img 读串口（含终端 `rust` 命令）===")
        qemu = args.qemu if (args.qemu and os.path.exists(args.qemu)) else find_tool(QEMU_CANDIDATES, "qemu-system-x86_64")
        if not qemu:
            return 2
        tmp = tempfile.mkdtemp(prefix="vimtu64_rust64_")
        serial_path = os.path.join(tmp, "serial.log")
        # ★ 用镜像**副本**启动：打开终端会幂等安装 /spin.elf、/filedemo.elf（真写盘），
        #   不能让 build64/system.img 被验收改脏（别的验收与构建还要用它）。
        img_copy = os.path.join(tmp, "system_copy.img")
        shutil.copyfile(args.img, img_copy)
        port = pick_port()
        if port is None:
            sys.stderr.write("找不到空闲的 monitor 端口\n")
            return 2

        def q(p):
            return p.replace("\\", "/")

        qargs = [
            qemu, "-name", "Vimtu64-rust64",
            "-drive", "format=raw,file=%s" % q(img_copy),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial_path),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ]
        print("[qemu] %s" % " ".join(qargs))
        proc = subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def slog():
            try:
                return read_text(serial_path)
            except OSError:
                return ""

        def wait_mark(needle, timeout):
            t0 = time.time()
            while time.time() - t0 < timeout:
                s = slog()
                if needle in s:
                    return True
                if proc.poll() is not None:
                    print("  [!] QEMU 提前退出（复位/三重故障？）")
                    return False
                time.sleep(0.25)
            return False

        try:
            # ---- 5a) 启动期自检行 ----
            found = RUST64_RX.search(slog())
            if not found:
                wait_mark("[RUST64] tokens ok", args.timeout)
            time.sleep(1.5)
            serial = slog()
            found = RUST64_RX.search(serial)
            ck.ok("串口出现 [RUST64] tokens ok 自检行（%.0fs 内）" % args.timeout, bool(found))
            if found:
                got_themes, got_accent, got_res = int(found.group(1)), int(found.group(2), 16), found.group(3)
                print("[raw] %s" % found.group(0))
                ck.ok("themes=%d == 源码主题数 %d" % (got_themes, declared), got_themes == declared)
                ck.ok("accent=#%06X == 源码 theme0 accent #%06X" % (got_accent, themes[0]["accent_rgb"]),
                      got_accent == themes[0]["accent_rgb"])
                ck.ok("selftest %s" % got_res, got_res == "PASS")
            boot_lines = [l for l in serial.splitlines() if RUST64_RX.search(l)]
            boot_hits = len(RUST64_RX.findall(serial))
            print("[raw] 启动期完整 [RUST64] 行（原文）：")
            for l in boot_lines:
                print("   | " + l)
            ck.ok("启动期 [RUST64] 自检行只打一次", boot_hits == 1, "count=%d" % boot_hits)

            # ---- 5b) 终端 `rust` 命令（C++ -> Rust 运行期接口的第二个调用点）----
            print("=== 6) 终端 `rust` 命令（Win 键 + 1 开终端；打点走串口）===")
            up = wait_mark("[GUI64] ready", 150)
            ck.ok("系统内核进桌面（[GUI64] ready）", up)
            mon = Mon(port)
            opened = False
            for _ in range(3):
                mon.key("meta_l", wait=0.9)
                mon.key("1", wait=1.8)
                if "[APP] term opened" in slog():
                    opened = True
                    break
            ck.ok("打开终端窗口（[APP] term opened）", opened)
            if opened:
                mon.type_line("rust")
                got_info = wait_mark("[RUST64] term rust cmd=info", 25)
                ck.ok("终端 `rust` 打到 Rust 接口（[RUST64] term rust cmd=info）", got_info)
                ck.ok("终端命令级打点 [TERM] cmd rust ok", "[TERM] cmd rust ok" in slog())
                if got_info:
                    mins = [l for l in slog().splitlines() if "term rust cmd=info" in l]
                    print("[raw] %s" % mins[-1])
                    mm = re.search(r"\[RUST64\] term rust cmd=info theme=(\d+) accent=#([0-9A-F]{6}) tokens=(\d+)", mins[-1])
                    ck.ok("rust 命令回读：tokens=%d（Rust 侧 token 表长度）" % token_count,
                          bool(mm) and int(mm.group(3)) == token_count)
                    ck.ok("rust 命令回读：accent 与源码 theme0 一致",
                          bool(mm) and int(mm.group(2), 16) == themes[0]["accent_rgb"])
                mon.type_line("rust tokens")
                got_tok = wait_mark("[RUST64] term rust cmd=tokens", 25)
                ck.ok("`rust tokens` 列出 Token 表（[RUST64] term rust cmd=tokens）", got_tok)
                mon.type_line("rust set 1")
                got_set = wait_mark("[RUST64] term rust cmd=set", 25)
                ck.ok("`rust set 1` 切换主题（[RUST64] term rust cmd=set）", got_set)
                if got_set:
                    ms = [l for l in slog().splitlines() if "term rust cmd=set" in l]
                    print("[raw] %s" % ms[-1])
                    mm = re.search(r"\[RUST64\] term rust cmd=set theme=(\d+) accent=#([0-9A-F]{6})", ms[-1])
                    ck.ok("切到暗色主题：theme=1 且 accent == 源码 theme1 accent",
                          bool(mm) and int(mm.group(1)) == 1 and int(mm.group(2), 16) == themes[1]["accent_rgb"])
                mon.type_line("rust set 0")
                ck.ok("主题切回 0（默认白色）", wait_mark("[RUST64] term rust cmd=set theme=0", 25))
        finally:
            if proc.poll() is None:
                proc.kill()
                try:
                    proc.wait(timeout=10)
                except Exception:
                    pass
        serial = slog()
        if args.keep_serial:
            with open(args.keep_serial, "w", encoding="utf-8") as f:
                f.write(serial)
        print("[raw] 全部 [RUST64] 行（原文）：")
        for l in [x for x in serial.splitlines() if "RUST64" in x]:
            print("   | " + l)
        ck.ok("无 Rust 侧 selftest FAIL", "selftest FAIL" not in serial)
        ck.ok("无 Rust panic（[RUST64] PANIC 未出现）", "[RUST64] PANIC" not in serial)
        ck.ok("无内核蓝屏（[PANIC64] 未出现）", "[PANIC64]" not in serial)
    else:
        print("=== 5) QEMU 阶段：--skip-qemu 跳过 ===")

    print("=== 结论 ===")
    good = ck.report()
    print("=== RESULT: %s ===" % ("PASS" if good else "FAIL"))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
