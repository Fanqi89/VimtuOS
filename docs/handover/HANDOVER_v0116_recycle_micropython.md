# HANDOVER —— v0.11.6 开发中（回收站 + MicroPython 脚本 + 程序窗口顶部两角 90° 弧）

- 日期：2026-11-25（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.6"`（**尚未发版** —— 本轮只做到构建 + 验收，发布脚本未跑）
- 基线：`main` 上一个已发布版本 = v0.11.5（tag `v0.11.5` → commit `2044d59d`，Release `456278688`）
- 本轮工作区：**未提交**（所有改动还在工作区，`git status` 有一批 M/??；见 §6）

> 本轮用户指令（原文顺序）：
> 1) 先确认 v0.11.5 状态；
> 2) **补齐回收站与删除机制**（右键删除后能进回收站，防止误删）；
> 3) **实现 MicroPython 小工具**（用 Python 写一个能在系统里直接运行的小程序，比如贪吃蛇或计算器）；
> 4) 有精力再推进窗口最大化时的内容自适应（Layout）；
> 5) 做完再处理 `桌面图标真实文件.txt` 与 `圆角渲染bug.txt` 里提到的问题。

## 1. 本批做成了什么

### 1.1 文件回收站（`kernel/trash64.h/.cpp` + explorer 集成）——**已完成并端到端验收**

- **新模块 `kernel/trash64.cpp`**（约 600 行，只进系统内核）：
  - `trash64_put64(vol, path, …)`：删除 = **同卷改名**进 `/Recycle`（`fs64_rename_to64`），
    一个字节都没动、不需要空闲空间翻倍；撞名退化成 `stem (2)ext`（99 次以内），**绝不覆盖**。
  - 元数据 `/Recycle/.meta/<条目名>.txt`（一行 `orig=<原路径>` + 一行 `type=file|dir`）。
    为什么一条一个文件而不是总索引：总索引要"读-改-写"整块文件，中途失败会连带毁掉别的条目；
    一条一个文件时，元数据写失败可以**把条目改名搬回去**（`[TRASH64] put rollback …`）→ 绝不静默丢数据。
  - `trash64_restore64`：搬回原路径；目标已存在 → `why=exists`（不覆盖），原父目录没了 → `why=noparent`。
  - `trash64_purge64`：**有界递归**真删（深度 ≤ 4、条目 ≤ 96、单层 ≤ 16 个孩子）；超界 → `why=budget` 且保留未删部分。
  - `trash64_empty64`：逐条 purge，循环到列表空或"一条都删不掉"（不死循环），如实回报失败条数。
  - `trash64_boot_ensure64(vol)`：**启动期以 root 建好** `/Recycle` 与 `/Recycle/.meta` 并 `chmod 0777`。
    为什么必须在 ring0 做：普通用户（uid 1000）在 `/` 上建不了目录（`[PERM64] deny op=create need=wx`），
    等到第一次删除再 mkdir 会一律 `-EACCES`。实测证据：`[TRASH64] boot ensure vol=0 dir=/Recycle created=1 mode=0777 ok=1`。
  - `trash64_selftest64()`：启动期在 `/tmp` 造样本走一遍 put → list → restore → purge（5 个 mask 位）。
    本轮实测 `[TRASH64] selftest SKIP reason=cannot-create-sample` —— 因为自检跑在 **登录前**、
    卷里还没有 `/tmp`（`/tmp` 由会话建立时创建）。**这是如实降级，不是假装通过**；真实验收在 fileops64_test 里。
- **explorer 集成**（`kernel/explorer64.cpp`）：
  - 删除走 `trash64_put64`（**非空目录现在能删**：一次改名，与内容多少无关）。
  - 视图判据只有一条：**当前路径 == `/Recycle`**。于是导航/历史栈/后退/前进/刷新全部自动正确。
  - 导航窗格**追加最后一行**「回收站」（节点号 = 驱动器数 + 1，驱动器节点号一个都没变）。
  - 回收站视图里**同一组工具栏按钮换了语义**：0 号 = 恢复、1 号永久删除、2 号清空（几何/命中 id 一字不改）。
  - 右键菜单同样按 `g_trash` 换成「恢复 / 永久删除 / 打开原位置 / 属性 / 刷新 / 清空回收站」。
  - 三件事都**两段式确认**（10 秒窗口，`g_del_confirm` 现在装的是"哪一类"，互不串味）。
  - 状态栏在回收站视图里明说"（回收站）"，选中条目时补一句 `<- <原位置>`；属性面板的"路径"给**原位置**。
  - `.meta` 目录在回收站视图里不列给用户（点开头 = 隐藏）。

### 1.2 MicroPython 跑脚本文件（`/mpy/*.py`）——代码完成，验收待跑（见 §3）

- `user/micropython/port/main.c`：
  - `argc > 1` → `pyexec_file(argv[1])`（上游同一函数，自带 nlr，脚本抛异常不会打死解释器）；
    无参数 → 老 REPL，一条字节不变。
  - `mp_lexer_new_from_file` 从"抛 ENOENT 的桩"改成**真实现**：musl `fopen/fread` 读整文件（≤1 MiB）
    进 `m_new` 缓冲，`free_len` 交给 lexer（`mp_reader_mem_close` 会 `m_del`），不泄漏。
- `kernel/terminal64.cpp`：`mp <脚本>` 把脚本路径经 `proc64_start_elf64_argv64` 摆成 `argv[1]`；
  打点 `[TERM] micropython path=/bin/mp pid=N rc=0 script=/mpy/calc.py`（**script= 放在最后**，
  老的 `micropython64_test` 正则按前缀匹配，因此不受影响）。
- `tools/micropython_pack_win.py` 新增 `--script-dir`：把 `user/micropython/demo/*.py` 装进 `/mpy/`（逐字节回读自检）。
- 示例脚本：
  - `user/micropython/demo/calc.py` —— 命令行计算器（交互 + `mp /mpy/calc.py 7*6` 单次）。
  - `user/micropython/demo/snake.py` —— 终端贪吃蛇（**回合制**：内核终端是行缓冲的，没有 termios/select，
    做不到实时按键；这一点写在脚本头注释里，不假装）。

### 1.3 程序窗口"顶部两角 90° 弧"（`圆角渲染bug.txt` 第一部分）——代码完成，像素验收待跑

- 根因（实测代码）：`gfx64_glass64()` 对**标题带**（高 27px）做四角圆角时把半径**夹到 h/2 = 13**，
  于是左右两块各自成了 180° 半圆端帽、底边两角也被啃掉。ring3 侧 `wm_corner_cut` 切四角 + `WM_DECO_R=14`
  同样让顶部弧看起来像端帽。
- 修法（**统一窗口合成层，一处改动对所有程序窗口生效**）：
  - `kernel/gfx64.cpp/.h` 新增两个原语：
    - `gfx64_glass_top64(...)`：标题带玻璃，**只有上面两角是弧**，r 不再夹到 h/2；
    - `gfx64_corner_cut_mask64(..., mask, ...)`：按位选角裁切（bit0 左上 / bit1 右上 / bit2 左下 / bit3 右下）。
  - `kernel/gui64.cpp`：`gui64_deco_frame64` → `gfx64_glass_top64`；`gui64_deco_edge64` → mask=0x3（**只切上两角**）。
  - `user/wm/wm.c`：`wm_corner_cut` → `wm_corner_cut_mask`，`wm_deco_edge` 用 0x3。
  - **系统栏（Dock / 任务栏）不调用这两个函数** —— 它们的形状一个像素都没动。

## 2. 验收（本轮实跑）

| 脚本 | 结果 |
|---|---|
| `tests/fileops64_test.py`（阶段 10/10.5 = 回收站往返） | 第一轮：回收站链路**全部 PASS**；5 条假红是我新写判据自己的错（已修）。**最后一次构建后未复跑** |
| `tests/micropython64_test.py`（阶段 ⑦ = 脚本执行） | REPL ①–⑥ **全 PASS**；**⑦ 未通过**（详见下）；`⑦ 无 PANIC / selftest FAIL` **PASS** |

### MicroPython 脚本执行现状（如实；UNVERIFIED 的部分点名）

- **已在卷里**：`/mpy/calc.py`、`/mpy/snake.py`（`tools/micropython_pack_win.py --script-dir` 逐字节回读自检通过）。
- **代码路径已通**：`mp <脚本>` → `proc64_start_elf64_argv64`（日志 `via=execve`）；
  端口 `argc>1 → pyexec_file(argv[1])`；`mp_lexer_new_from_file` 已从"抛 ENOENT 的桩"改成真读文件。
- **未通过的那一条**：注入 `mp /mpy/calc.py 7*6` 时内核只看到 **argc=1**（`[MPDRV] start path=/bin/mp argc=1`），
  即**参数没进到内核命令行**（连敲 3 次结果相同）→ `script=` 打点与 `7*6 = 42` 都没出现。
  同会话里 `df`（无参数）注入正常，所以注入通道本身是通的；**怀疑点**：终端行编辑对 `/` `.` `*`
  的注入（QEMU `sendkey slash/dot/shift-8`）或 `g_arg1` 取值时机。**尚未定位**。
- 下一步定位（按序）：① 手工敲 `ls /mpy`（应 entries=2）证明脚本在卷里；② 敲 `mp`（无参）看 REPL 照常起；
  ③ 逐字符注入，找出是哪个键被吞。

### 第一轮 fileops64 的 5 条假红（已修，都是我的判据写错）

1. `last_item_idx()` 扫整份日志 → 删除**之前**的 item 行让它一直返回旧下标（两条）。改成"本次删除之后不再出现该行"。
2. 清空的第二次点击依赖"右键菜单真的重新打开"（两次都只打 `empty ask`，清空没执行）→ 改用工具栏 2 号按钮。
3. 阶段 10.5 收尾停在 `/docs`，把阶段 11 的 `mkdir path=/newdir` 变成假红 → 收尾补一次"上级"退回盘根。

### 回收站的真实证据（第一轮串口原文）

```
[TRASH64] boot ensure vol=0 dir=/Recycle created=1 mode=0777 ok=1
[TRASH64] put name=renamed.txt    orig=/docs/renamed.txt    type=file vol=0 rc=0 why=- meta=1
[TRASH64] put name=nonempty       orig=/nonempty            type=dir  vol=0 rc=0 why=- meta=1
[TRASH64] view open vol=0 items=3 path=/Recycle
[TRASH64] purge name=nonempty type=dir files=1 dirs=1 rc=0 why=-      ← 有界递归真删
[TRASH64] restore name=renamed(2).txt to=/docs/renamed(2).txt rc=0     ← 搬回原位
[TRASH64] empty ask items=1 confirm=1                                  ← 清空二次确认
```

## 3. 未完成 / 下一步（按序）

1. **重跑 `tests/fileops64_test.py`**（本批最后一次构建后）——确认 100+ 条全绿。
2. **跑 `tests/micropython64_test.py`**（已加阶段 ⑦：`mp /mpy/calc.py 7*6` → `7*6 = 42`）——
   若 MicroPython 的 `fopen` 在 musl + 本内核上还有坑（fstat/缓冲），会在这里暴露；
   失败就退到"端口侧用裸 `open/read/lseek` 读"的写法（注释里已写清退路）。
3. **程序窗口圆角像素验收**（尚未做）：`deco64_test` 只验几何；需要一个截图判据
   （左上/右上角弧外是壁纸、左下/右下角是窗口像素、顶部弧下方是标题带）。
4. **`桌面图标真实文件.txt`（桌面 = 真实文件夹）——未做**。这是本批唯一整条没动的大项，方案已明确：
   - 桌面项的真源从 `/etc/desktop.links` 改成**列 `/home/vimtu/Desktop`**；`.links` 退化成"位置缓存"（只存 x/y）；
   - 拖拽到桌面 = `fs64_rename_to64` 真移动（现有 `desktopops64_add_file64` 只登记路径，要改成移动）；
   - 实时刷新：`desktopops64_tick64` 里按 mtime/条目数变化重列（现在只同步 config）；
   - 图标按 kind 匹配（`.txt` → 文本图标；自家应用/第三方应用用自己的图标）。
   - 注意：这会与"桌面回收站"（`desktopops64` 里那套**内存 + /etc/desktop.links** 的快捷方式回收站）
     撞车 —— 文件级删除已经走 `/Recycle`，两套要合并成一套（建议桌面回收站图标直接开 explorer 的 `/Recycle`）。
5. **`圆角渲染bug.txt` 第二部分（开始菜单左上角时间块色差）——未做**。
6. 版本号已 bump 到 0.11.6 但**未发版**（未跑 `rel_*.py`、未建 tag/Release）。

## 4. 本批改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/trash64.h/.cpp` | **新增**：回收站核心（put/restore/purge/empty/boot_ensure/selftest，`[TRASH64]` 打点） |
| `kernel/explorer64.cpp` | 删除→回收站；`g_trash` 视图；导航窗格追加"回收站"；工具栏/右键菜单/状态栏/属性按视图换语义；三处两段式确认 |
| `kernel/explorer64.h` | `explorer64_open_trash64()` |
| `kernel/kernel64.cpp` | 启动期 `trash64_boot_ensure64` + `trash64_selftest64` |
| `kernel/gfx64.h/.cpp` | 新增 `gfx64_glass_top64` / `gfx64_corner_cut_mask64` |
| `kernel/gui64.cpp` | 标题带改顶部两角；窗口裁切 mask=0x3 |
| `user/wm/wm.c` | `wm_corner_cut_mask`，程序窗口只切上两角 |
| `kernel/terminal64.cpp` | `mp <脚本>` → argv 传脚本；打点带 `script=` |
| `user/micropython/port/main.c` | `argc>1` 跑脚本 + 真 `mp_lexer_new_from_file` |
| `user/micropython/demo/calc.py` / `snake.py` | **新增**示例脚本 |
| `tools/micropython_pack_win.py` | `--script-dir` → `/mpy/*.py` |
| `build64.sh` | 0.11.5→0.11.6；`SRCS_DESKTOP` 加 `trash64.cpp`；两处链接加 `trash64.o`；mp pack 带 `--script-dir` |
| `tests/fileops64_test.py` | 阶段 10 判据按实现重写（非空目录删除成功 + 进回收站）+ 新增阶段 10.5 回收站往返 |
| `tests/micropython64_test.py` | 新增阶段 ⑦ 脚本执行 |

## 5. 不要踩的坑（本批新增）

1. **`fs64_stat64(vol, path, nullptr)` 直接返回 -1**（它开头就 `if (!out) return -1;`）——
   "存在与否"必须传一个真结构体（`trash64.cpp` 里统一走 `tr_missing()`）。
2. **`/` 上普通用户建不了目录**：回收站目录必须在 ring0 启动期建（`trash64_boot_ensure64`），
   否则第一次删除一律 `-EACCES`。
3. **`last_item_idx()` 会扫到删除前的旧行**：判"条目已消失"必须限定 `log[since:]`。
4. **右键菜单二次点击不可靠**：第一下已经把菜单关了，再"右键 + 点同一坐标"要赌菜单真的重开；
   验收优先用工具栏固定坐标（同一段逻辑）。
5. **Edit 工具按行号改代码，行号漂移会改错地方**：本轮踩了三次（`trash64.cpp` 一处、`explorer64.cpp` 三处、
   `terminal64.cpp` 一处）。稳妥做法：长块用 `$env:PI_SCRATCH_DIR\splice.py`（行区间 + 首末行断言），
   搬块用 `move_block.py` / `to_eof.py`（都带断言，不满足就 abort 不写盘）。
6. **块插在 `#include` 之前会全篇报"unknown type name"**：`gfx64.cpp` 的新原语最终落在**文件末尾**
   （它们用到文件内静态助手：`rect_clip64/panel_blur64/canvas64/cov_table64/blend_c64`）。
7. **改 `cmd_micropython` 签名时别把 `if (ts->r3_pid > 0) {` 一起替换掉**（本轮就这么把函数头啃了，
   直到构建才暴露 —— 改完函数头**立刻**跑一次 `syncheck2.sh`）。

## 6. 现场

- 内核 `build64/kernel64_os.bin`：上一轮（含回收站全量）**3,472,432 B**；加本批圆角 + MicroPython 后见构建日志
  （闸门 15,728,640 B）。
- 构建：`C:\msys64\usr\bin\bash.exe -lc "bash build64.sh"`（封装脚本 `$env:PI_SCRATCH_DIR\run_build.sh`，日志 `build.log`）。
- 语法自检：`$env:PI_SCRATCH_DIR\syncheck2.sh <文件...>`（clang `-fsyntax-only`，PATH 需含 `/mingw64/bin`）。

## 7. 收尾（v0.11.6 定稿）

### 7.1 MicroPython 参数的**真因**已定位并修好（不再是 UNVERIFIED）

用户提示"大概率是终端切分或 execve 少算参数"。实测**都不是** —— 参数一直是好的：

```
[TERM] micropython path=/bin/mp pid=27 rc=0 (async; keys go to fd 0) script=/mpy/calc.py
[MPDRV] start path=/bin/mp argc=2 argv0=/bin/mp
[FD64] open path=/mpy/calc.py fd=3 flags=32768 vol=0            ← 脚本从 VimtuFS2 读进来
VimtuOS 计算器 (MicroPython)                                     ← 中文源码编译执行、打印成功
Traceback (most recent call last):
  File "/mpy/calc.py", in <module>
AttributeError: 'module' object has no attribute 'exit'        ← **真因 1**
```

真因是**端口配置**，不是传参：

1. `MICROPY_PY_SYS_EXIT = 0` → 脚本第一版结尾 `sys.exit(...)` 直接 `AttributeError`；
2. `MICROPY_PY_BUILTINS_INPUT = 0`（ROM 级别是 CORE_FEATURES，upstream 默认值就把它关掉了）
   → 修好 exit 以后，`input("calc> ")` 又 `NameError: name 'input' isn't defined`，
   脚本打印完标题就退出（症状："只有标题行、没有结果"）。
3. 另外 `MICROPY_PY_SYS_ARGV = 0` → 脚本拿不到命令行参数，所以 calc.py 改成**纯交互**，
   并把这条边界写在脚本头注释里（不假装单次模式能用）。

修法（`user/micropython/port/mpconfigport.h`）：打开 `SYS_EXIT` 与 `BUILTINS_INPUT`，
`ARGV` 保持关闭；脚本 `user/micropython/demo/calc.py` 去掉 sys/argv 依赖。

**验收（实跑 PASS）**：`tests/micropython64_test.py` → **PASS**
（⑦ 脚本 argv=2 + `script=`、⑦b 脚本真跑、⑦c 终端出现 `7*6 = 42`、⑦d `q` 退出解绑、⑦e 键盘回 shell、
全程无 PANIC/selftest FAIL）。

### 7.2 `fileops64_test` 复跑干净 — PASS 117/117

顺带修掉两条**判据**问题（都不是产品缺陷）：
- `last_item_idx()` 扫整份日志会命中"删除前"的旧行 → 改成"本次动作之后不再出现该行"；
- 清空的第二次点击用 `mouse.click` 会先 `blind_goto` 逐像素挪光标（实测吃掉十几秒），
  10 秒确认窗口就在这里过期 —— 改成"光标不动、原地再点一次"（`exp.single_click_at`）。
  **这条是真发现**：确认窗口对"手慢的用户"也算紧，但产品不改（安全优先），验收按真实操作改。

### 7.3 交接单目录整理（用户第 1 条指令）

- 9 份历史交接单 + 本单全部移进 **`docs/handover/`**（`git mv`，历史可 `--follow`）；
- 仓库根目录只留**索引** `HANDOVER.md`（+ README/FONTS/构建说明）；
- 索引与 `docs/发布流程.md` / `docs/项目状态总览.md` 里 26 处引用一次性改成 `docs/handover/…`；
- 新增 `docs/handover/README.md`（命名与流程的硬规定）+ `docs/发布流程.md` §5 加一条发布前检查：
  **交接单一律写进 `docs/handover/`，根目录不许再出现 `HANDOVER_v*.md`**。

### 7.4 下一轮**最高优先级**（用户指定）

**桌面即真实文件夹（`/home/vimtu/Desktop/`）** —— 详见 §3 第 4 条的方案；要点：
桌面项真源 = 列 `/home/vimtu/Desktop`；`.links` 退化成位置缓存；
拖到桌面 = `fs64_rename_to64` 真移动；`desktopops64_tick64` 里做实时刷新；
图标按 kind 匹配（`.txt` → 文本图标，自家/第三方应用用自己的图标）；
并**合并两套回收站**（桌面回收站图标直接开 explorer 的 `/Recycle`）。

### 7.5 现场（定稿）

- `kernel64_os.bin` **3,475,104 B**（闸门 15,728,640 B）；`build64.sh` `VIMTUOS_VERSION="0.11.6"`；
- 构建 `EXIT=0`；`micropython.bin` 体积随 INPUT 打开略增（见构建日志"卷自检 OK"）；
- **未提交前的最后一件事**：全部改动一次提交（含 `docs/handover/` 的移动），工作区干净。
- **未发版**（用户明确"暂缓发版"）：tag/Release 都没建。
