# third_party/tcc - VimtuOS 里的 TinyCC（A4-2b）

## 这是什么

上游 **TinyCC 0.9.27**（Fabrice Bellard 等，2001-2017）的源码子集，供 VimtuOS 的
A4-2b 批次在 ring3 里跑一个 C 编译器用：

- 上游 tarball：`https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27.tar.bz2`
  （634,999 B；本仓库放的是**构建/运行真正需要的那部分源码**，不是整包）
- 取进来的文件：`COPYING` `RELICENSING` `README` `VERSION` `Changelog` `CodingStyle`、
  `tcc.c` `libtcc.c` `libtcc.h` `tcc.h` `tccpp.c` `tccgen.c` `tccelf.c` `tccasm.c`
  `tccrun.c` `tcctools.c` `tccpe.c` `tcccoff.c`、`x86_64-gen.c` `x86_64-link.c`
  `i386-asm.c` `i386-asm.h` `i386-tok.h` `x86_64-asm.h`、`elf.h` `stab.h` `stab.def`
  `tcctok.h` `tcclib.h`、`include/*.h`（tcc 自己的 5 个最小头）、`lib/libtcc1.c`
- 没取进来的：`tests/`、`examples/`、`win32/`、`tcc-doc.*`、其它架构的 `*-gen.c`
  （构建不用它们；上游整包随时可从上面那条 URL 取）

## 许可（原文随源码一起放）

- **主许可：GNU LGPL v2.1+** —— 见 `COPYING`（原文，未改动）与每个源文件头部的声明。
- **`lib/libtcc1.c` 是例外**：文件头写明它是 GPLv2+ **并附加"可无限制链接进其它程序"的例外条款**
  （libgcc 的同类条款），所以把它编成 `libtcc1.a` 静态链进用户程序没有额外限制。
- 另见 `RELICENSING`：上游声明了"把整体改授 MIT"的意图与各文件作者名单。
- 本仓库只是**原样取用 + 少量必要补丁**（下面逐条列出），没有改变许可。

## 打过的补丁（全部**在位**在源码里，不是构建脚本里 sed 出来的）

| 文件 | 改动 | 为什么（每条都有实测/结构原因） |
|---|---|---|
| `config.h` | **重写**（上游由 configure 生成）：`CONFIG_TCCDIR "/tcc"`、`CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"`、`CONFIG_TCC_LIBPATHS "{B}/lib"`、`CONFIG_TCC_CRTPREFIX "{B}/lib"` | 我们不跑上游 configure（理由见 `tools/tcc_build_win.sh` 头部）。这四个路径必须指向**系统卷里的 /tcc 树** —— 内核区只剩 ~187 KB，tcc 与它的头/库一个字节都不许进内核镜像 |
| `x86_64-link.c` | `ELF_START_ADDR 0x400000 -> 0x100000000` | VimtuOS 的用户窗口在 **4 GiB**（kernel/usermode64.h 的 `USER64_CODE_VA64`），内核 elf64.cpp 会拒绝落在 `[4GiB,4GiB+64KiB)` 之外的 PT_LOAD。上游默认 4 MB -> 产出的可执行文件在本内核里装载不了 |
| `x86_64-link.c` | `ELF_PAGE_SIZE 0x200000 -> 0x1000` | 它被 `layout_sections()` 当"段/文件偏移对齐"用：2 MiB 对齐会让产物文件凭空多出 ~2 MB 空洞，且段间距 2 MiB 直接越出 64 KiB 装载区。改成内核的页大小 4 KiB 后段是紧凑的 |
| `libtcc.c` | `tcc_new()` 里加 `s->static_link = 1;` | 上游在 Linux 上**默认产出动态可执行**（PT_INTERP + PT_DYNAMIC + .plt），而本内核的装载器只装静态 ELF64：带 PT_INTERP 的产物会先去找 `/lib64/ld-linux-x86-64.so.2`（本系统没有）-> 被拒。命令行上显式 `-static` 与这一句完全等价 |
| `tccpp.c` | `TOKSYM_TAL_SIZE/TOKSTR_TAL_SIZE/CSTR_TAL_SIZE = 768K/768K/256K -> 8K/8K/2K` | 这三个"小块分配器"是 tcc **一启动就一次性分配**的固定底数，合计 **1,835,008 B**（宿主侧 `-DMEM_DEBUG=1 -bench` 实测：编一个 19 行、无头的 hello.c 峰值堆 1,872,128 B —— 98% 是这块底数）。1 MiB 的窗口里这是必然 OOM。改小之后同一输入的 `-c` 峰值堆降到 ~169 KB。功能无损：这三个竞技场本来就会 `tal_new()` 翻倍长大（上游设计如此），初始值只影响"要不要长" |
| `tcc.h` | `TOK_HASH_SIZE 16384 -> 1024` | `hash_ident[]` 是 8 B/项的静态数组：16384 项 = **128 KiB 的 .bss**（tcc 映像 span 里实打实的一块），1024 项 = 8 KiB。功能不变（只是冲突率略高） |
| `tccelf.c` | 静态 EXE 分支里加 `relocate_plt(s1)` 与新的 `fill_got_static(s1)` | **上游的静态链接在 x86_64 上是坏的**：`R_X86_64_PLT32` 是 ALWAYS_GOTPLT_ENTRY（x86_64-link.c:101），只要调用**全局**函数就会生成 `jmp *disp(%rip)` 形式的 PLT 桩；而 `relocate_plt()`（修 disp）只在 `if (dynamic)` 分支里调、PLT 的 GOT 槽只由动态链接器（`R_X86_64_JUMP_SLOT`）填 —— 静态产物因此整片 .got = 0、PLT 跳到 .plt 中间的垃圾字节（实测 ring3 里 `rip=0x2825FFFFFF` 直接 PANIC）。本补丁：静态分支补一次 `relocate_plt()`，并用 GOT 自己的 reloc 段把每个槽写成符号最终地址 |

## 内存账（为什么 ring3 里只走到"启动期 OOM"）

`kernel/usermode64.h` 的用户窗口**整个只有 1 MiB**（装载区 64 KiB + 用户栈 16 KiB +
`brk` 64 KiB + `mmap` 448 KiB）。本批把 tcc 的"固定内存底数"从上游的 **1.87 MB** 压到
**~169 KB**（两处补丁：三个小块分配器、`hash_ident`），并把映像 span 压到 **276 KiB**
（`-Oz` + `-mcmodel=large`，实测 `build64/tcc.bin` = 282,328 B、span 282,624 B）。
但 448 KiB 的 mmap 区减去 276 KiB 映像与 24 KiB 的栈之后只剩 **~148 KiB 堆**，
仍然小于 ~169 KB —— 所以 `run /bin/tcc -v` 会在**启动期**打 `tcc: error: memory full (malloc)`
（驱动那一层全部正常：load/mmap/mprotect/stack/auxv/jmp 都有串口打点）。要让它在 ring3 里
真的编出 `.o`，需要**把用户窗口做大**（内核侧常量）或**把 tcc 的分配器换成零开销的
bump 竞技场**（musl 的 mallocng 有元数据与分组浪费）—— 两条都超出 A4-2b 允许改的范围。
## 相关文件（不在本目录）

| 文件 | 作用 |
|---|---|
| `tools/tcc_build_win.sh` | 手写构建：tcc 本体 / libtcc1.a / crt1.o / 极小 libc.a / 装载驱动 / 宿主校验器 |
| `tools/tcc_pack_win.py` | 把 `/tcc/**`、`/bin/tcc`、`/lib/tcc.bin`、`/hello` 装进 VimtuFS2 系统卷（含二级间接） |
| `user/apps/tcc/tccdrv.c` | ring3 装载驱动（= 卷里的 `/bin/tcc`）：mmap + 搬段 + 改 auxv + jmp |
| `user/apps/tcc/tcc_start.{S,c}` | tcc 自己的 `_start`（musl 的 crt1.o 在 4 GiB 基址下链不起来） |
| `user/apps/tcc/tccmini.{c,start.S}` | 给 **tcc 产物**用的极小 libc / crt1.o（`/tcc/lib/libc.a`、`/tcc/lib/crt1.o`） |
| `tests/tcc64_test.py` | 端到端验收（tcc 在 ring3 跑起来 / -c / 产物能跑 / 路径来自系统卷 / `run > 文件`） |
