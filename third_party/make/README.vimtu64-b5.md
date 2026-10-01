# third_party/make —— GNU Make 4.4.1（VimtuOS ★ B5 的第三方源码）

来源与许可
==========

* 上游：GNU Make 4.4.1（https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz，2023-02-26）。
* 许可：**GPL-3.0-or-later**（原文本见本目录的 `COPYING`；上游 README/NEWS/INSTALL/AUTHORS 一并保留）。
  VimtuOS 只是**链接**这份源码（静态可执行文件放在系统卷里，作为独立程序运行）——按 GPL 的口径，
  分发该二进制时需要给出对应源码，本目录就是那份源码。
* 本仓库收录的范围：`src/**`（28 个翻译单元 + 头）、`lib/**`（gnulib 模块：`concat-filename.c`
  `findprog-in.c` `fnmatch.c` `glob.c` `getloadavg.c` 与配套头）、以及 `COPYING/README/AUTHORS/NEWS/INSTALL`。
  **未收录**：`configure`/`Makefile.in`/`doc/`/`po/`/`m4/`/`tests/`/`build-aux/`（VimtuOS 不走 autotools：
  见 `tools/make_build_win.sh` 的"手写 config.h + 手写编译行"说明）。
* 注意：`lib/glob.h` / `lib/fnmatch.h` 在上游是**构建期生成**的（由 `*.in.h` 改名，无占位符）；
  本目录只留 `.in.h`，构建脚本负责改名（见 `tools/make_build_win.sh` 第 1 步）。

构建（VimtuOS 目标）
====================

    bash tools/make_build_win.sh build64

产物（全部进系统卷，**一个字节都不进内核镜像**；内核里也搜不到它们的 64B 探针）：

| 产物 | 大小（本批实测） | 卷内路径 | 说明 |
|---|---|---|---|
| `build64/make.bin` | 309,536 B | `/lib/make.bin` | 真 make（静态 musl、非 PIC、按 4GiB+0x90000 定址） |
| `build64/make` | 17,240 B | `/bin/make` | 装载驱动（< 64 KiB，内核主程序装载器直接装） |

配置与目标平台补丁（详见工具脚本头部注释）：

1. **config.h 手写**：不跑 `configure`（它要在宿主上**运行**测试程序；实测 MSYS2 上它对 fork/waitpid/
   sys/wait.h 的结论还是错的）。改为：`src/config.h.in` 的 207 个 `#undef` 槽位 + 一张"musl 有 +
   VimtuOS 内核有系统调用"的取值表（89 项），其余保持未定义（gnulib 的无条件定义原样保留）。
2. **dir.c 的目标平台补丁**：ring3 没有 `getdents(217)`（内核只在 shell 的邮箱协议里代列目录），
   `readdir()` 一律 `-ENOSYS`。补丁把 `file_exists_p` 退化为 `stat()` 判存在：
   隐式规则（`%.o: %.c`）与 VPATH 的存在性判定照常工作；`$(wildcard)`/glob 不可用（如实标注）。
   补丁只打在 `build64/make_obj/makesrc` 的**副本**上，本目录里的源码保持上游原样。
