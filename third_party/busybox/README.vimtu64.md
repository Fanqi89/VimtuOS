# third_party/busybox —— 来源、许可、与本仓 GPL-3.0 的兼容性判断

## 来源（可复现）

* 上游：<https://busybox.net/downloads/busybox-1.36.1.tar.bz2>（2023-03-01 发布，sha 见上游）
* 本地落地：`third_party/busybox/busybox-1.36.1.tar.bz2`（2,525,473 B，原样保存）+
  解压出来的 `third_party/busybox/busybox-1.36.1/`（**未打任何补丁**）
* 构建：`tools/busybox_build_win.sh` —— 每次都把源码树**全新拷到 `build64/busybox_obj/src`**
  再打"宿主工具补丁"（MSYS2/mingw 的 fixdep/split-include/kconfig/confdata/applet_tables/
  mkconfigs）。所以本目录永远是上游原样，补丁只存在于构建副本里（每处都标了
  `VIMTU64 HOST PATCH`）。下载地址不可达时的替代：同目录的 tarball 直接解压即可。

## 许可

* **busybox 是 GPL-2.0**（上游 `LICENSE`，全文 18,348 B 已原样拷成 `third_party/busybox/LICENSE`）。
* 上游 `LICENSE` 顶部明确写着："This program is free software; you can redistribute it
  and/or modify it under the terms of the GNU General Public License as published by the
  Free Software Foundation; **either version 2 of the License, or (at your option) any later
  version**." —— 即 **GPL-2.0-or-later**（`libbb`/多数文件标 GPLv2+，个别文件另有说明）。
* 本仓（VimtuOS/Vimtu64）整体是 **GPL-3.0**。

## 兼容性判断与做法（本批）

1. **兼容**：GPL-2.0-**or-later** 允许按 GPL-3.0 的条款再分发，而 GPL-3.0 与 GPL-3.0 不冲突。
   因此"把 busybox 源码放进本仓、并用它构建一个交付进系统卷的二进制"在许可上是允许的：
   * 源码：本仓已经带着上游 `LICENSE` 原文（GPL-2.0 全文）与版权头；
   * 二进制（`/lib/busybox.bin`，交付进系统卷）：它是 GPL 程序的可执行形式，随发行物一起
     提供的源码 = 本目录（同一份 tarball + 同一份打补丁的构建脚本，可完整重建）。
   * **注意**：本目录的文件**不重新署名**、不改标 GPL-3.0；它们的许可仍然是
     GPL-2.0-or-later（向上游看齐）。GPL-3.0 是本仓**自己的**代码/文档的许可。
2. **GPL-2.0-only 才有的问题**：若某个文件被标成 `GPLv2 only`，它与 GPL-3.0 的组合分发会有
   摩擦。本移植**没有**把 busybox 的代码静态链接进任何"只允许 GPL-3.0"的产物里 ——
   `busybox.bin` 是**独立可执行文件**（系统卷里的一个文件，和内核、和别的用户态程序之间是
   进程边界），构建期拷贝出来的对象也只进这**一个**二进制。上游 1.36.1 下没有发现
   `GPLv2 only` 的头文件被复用进本仓自有代码。
3. **本仓自有部分**（`user/busybox/**`、`tools/busybox_build_win.sh|pack_win.py`、
   `tests/busybox64_test.py`、文档）按本仓许可（GPL-3.0）发布 —— 它们是**我们写的**，
   与 busybox 之间是"调用/打包"关系（execve 边界与构建期取源），不是代码级混合。
4. 报告里如实写：以上是工程判断，不构成法律意见；若要对外发布二进制，按 GPL-2.0-or-later
   的要求随附源码与许可原文即可（本目录已经满足）。
