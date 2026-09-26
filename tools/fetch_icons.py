#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/fetch_icons.py - 把宽松许可图标套件的 **SVG 源文件 + LICENSE 原文** 取到
gui_rs/assets/ 下（一次性/可复现；不参与内核构建，产物已入库，构建时无需联网）。

为什么只选这两套（法律与内核能力双重理由）：
  * Bootstrap Icons (MIT) —— 纯填充路径（fill-only），几何简单，自研光栅器（tools/svg2png.py）
    能完整覆盖；图标原名 -> 本仓库稳定名（kind 名）的映射见下表 MAP。
  * Material Icons (Apache-2.0) —— 只取它的 **图标字体 ttf** 放进 gui_rs/assets/fonts/：
    目前的 64 位内核**没有图标字体渲染能力**（font.cpp 只做 TrueType 字形光栅化 + 文本码位），
    所以字体只入库备将来用（见 assets/fonts/README.md 的说明），不参与本轮渲染。

许可（逐字原文都落盘）：
  * Bootstrap Icons : MIT                     https://github.com/twbs/icons        -> assets/icons/LICENSE
  * Material Icons  : Apache-2.0              https://github.com/google/material-design-icons
                                                                                  -> assets/fonts/LICENSE-MaterialIcons.txt
禁用清单（本脚本绝不取用）：GPL/AGPL 图标集（如 Papirus）、需要商标授权的品牌 logo。

用法：py -3 tools/fetch_icons.py [--check]
      --check 只校验已入库的文件是否齐全（离线；CI/构建前置检查用）
"""
import argparse
import hashlib
import os
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "gui_rs", "assets")

RAW_BI = "https://raw.githubusercontent.com/twbs/icons/main/"
RAW_MDI = "https://raw.githubusercontent.com/google/material-design-icons/master/"

# 本仓库稳定名 -> 上游文件名（Bootstrap Icons icons/*.svg）
MAP_SYSTEM = {
    "globe":            "globe",
    "ethernet":         "ethernet",
    "wifi":             "wifi",
    "volume":           "volume-up",
    "volume-mute":      "volume-mute",
    "bell":             "bell",
    "search":           "search",
    "close":            "x-lg",
    "power":            "power",
    "gear":             "gear",
    "lock":             "lock",
    "reboot":           "arrow-clockwise",
    "chevron-left":     "chevron-left",
    "chevron-right":    "chevron-right",
    "plus":             "plus-lg",
    "person":           "person",
    "usb":              "usb-drive",
    "monitor":          "display",
    "check":            "check-lg",
    "battery":          "battery-full",
}
MAP_APPS = {
    "mypc":       "pc-display",
    "recycle":    "trash",
    "terminal":   "terminal",
    "settings":   "sliders",
    "tmgr":       "cpu",
    "mines":      "grid-3x3-gap",
    "calc":       "calculator",
    "files":      "folder2-open",
    "monitor-app": "speedometer2",
    "about":      "info-circle",
}


def fetch(url, dst, tries=4):
    last = None
    for i in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=45) as r:
                data = r.read()
            if not data or len(data) < 32:
                raise IOError("empty/short body (%d B)" % len(data))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data)
            return len(data)
            with open(dst, "wb") as f:
                f.write(data)
            return len(data)
        except Exception as e:      # 网络抖动重试；失败要**明确报错**，绝不静默跳过
            last = e
            print("    retry %d/%d: %s (%s)" % (i + 1, tries, url, e))
    raise SystemExit("下载失败：%s（%s）" % (url, last))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验已入库文件齐全（离线）")
    args = ap.parse_args()

    want = []
    for name, src in sorted(MAP_SYSTEM.items()):
        want.append((os.path.join(ASSETS, "icons", "system", name + ".svg"), RAW_BI + "icons/" + src + ".svg"))
    for name, src in sorted(MAP_APPS.items()):
        want.append((os.path.join(ASSETS, "icons", "apps", name + ".svg"), RAW_BI + "icons/" + src + ".svg"))
    want.append((os.path.join(ASSETS, "icons", "LICENSE"), RAW_BI + "LICENSE"))
    want.append((os.path.join(ASSETS, "fonts", "MaterialIcons-Regular.ttf"),
                 RAW_MDI + "font/MaterialIcons-Regular.ttf"))
    want.append((os.path.join(ASSETS, "fonts", "LICENSE-MaterialIcons.txt"), RAW_MDI + "LICENSE"))

    miss = [p for p, _ in want if not os.path.exists(p)]
    if args.check:
        for p in miss:
            print("MISSING %s" % os.path.relpath(p, ROOT))
        print("check: %d/%d 齐全" % (len(want) - len(miss), len(want)))
        return 1 if miss else 0

    total = 0
    for p, url in want:
        n = fetch(url, p)
        total += n
        print("  %-58s %6d B  <- %s" % (os.path.relpath(p, ROOT).replace("\\", "/"), n, url))
    print("==> %d 个文件，共 %d 字节" % (len(want), total))

    # 记录逐文件 sha256 + 来源（可复现性；不含任何商标素材）
    man = os.path.join(ASSETS, "icons", "SOURCES.md")
    with open(man, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 图标资源来源（逐文件 sha256）\n\n")
        f.write("| 本仓库路径 | 上游 | 许可 | sha256 |\n|---|---|---|---|\n")
        for p, url in want:
            rel = os.path.relpath(p, ROOT).replace("\\", "/")
            f.write("| `%s` | %s | %s | `%s` |\n" % (rel, url, "MIT/Apache-2.0", sha256_file(p)))
    print("==> 已写 %s" % os.path.relpath(man, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
