# third_party/lua —— Lua 5.4.7（上游原样源码 + VimtuOS 构建说明）

本目录是 **Lua 5.4.7** 的官方发布源码，原样解压，**未做任何源码修改**（唯一的本仓库文件是
本 README）。VimtuOS 的构建脚本在 `tools/lua_build_win.sh`，它在 `lua-5.4.7/src/` 上直接
编译，因此**不需要**给上游打补丁。

## 来源

* 上游：<https://www.lua.org/ftp/lua-5.4.7.tar.gz>
* 下载时间（本仓库构建机）：2026-09-30
* SHA-256（下载到的 tar.gz，可用 `sha256sum` 复核）：
  `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`
  （若与本机重下的不同：以 lua.org 的 `lua-5.4.7.tar.gz` 为唯一来源，版本号必须仍是 5.4.7 ——
  构建脚本会核对 `src/lua.c` 里的 `LUA_VERSION_RELEASE`。）

## 许可（MIT）

Lua 是 MIT 许可的自由软件。原文（`doc/readme.html` 的 license 一节，逐字抄录）：

```
Copyright (C) 1994-2024 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## VimtuOS 构建（一句话）

`bash tools/lua_build_win.sh` → `build64/lua.bin`（解释器，静态 musl，钉在 4GiB+0x90000）+
`build64/lua`（装载驱动，< 64 KiB，走内核主程序装载器）。两个产物都只进**系统卷**，
内核镜像里一个字节都不加。

## 取舍（为什么用默认配置而不是 `LUA_USE_LINUX`）

`luaconf.h` 的默认配置只用 C89 libc；`LUA_USE_LINUX` 会额外要 readline（交互式行编辑）与
dlopen（`package.loadlib`），VimtuOS 两者都没有。因此本构建**不加** `LUA_USE_LINUX/POSIX`：

* 核心（lapi/lcode/lvm/…）只用到 `malloc/realloc/free/memcpy/…/strtod/snprintf/localeconv`
  —— musl 全部提供，实测可跑；
* `io.popen` 在默认配置下**明确报错**（"'popen' not supported"），不是静默失败；
* `os.execute` 走 musl 的 `system()`，而 VimtuOS 没有 `/bin/sh` → 它**如实失败**
  （返回非 0 状态），不会假装成功；
* 交互式 `-i` 仍可用（默认 stdin/stdout 读行，无 readline 历史/补全）。
