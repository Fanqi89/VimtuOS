# HANDOVER —— v0.11.7（桌面 = 真实文件夹 / 回收站合并 / 贪吃蛇真跑）

- 日期：2026-11-24（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.7"`（**未发版**：用户本轮只要求"打包一个测试镜像"，tag/Release 都没建）
- 基线：上一个已发布版本 = v0.11.5（tag `v0.11.5` → commit `2044d59d`，Release `456278688`）；v0.11.6 已提交未发版（`65626737`）
- 产物：`build64/sysdisk.img`、`build64/system.img`、`vimtu64-64.iso`（93,458,432 B）、`vimtu64-64.img`；构建 `EXIT=0`

> 用户本轮三条指令（原文顺序）：
> 1) **桌面即真实文件夹**：与 `/home/vimtu/Desktop/` 深度绑定，拖拽必须是真实文件移动，终端里新建文件桌面要实时刷新；
> 2) **MicroPython 实战验证**：把 `snake.py` 跑起来，截图；
> 3) **验证回收站与圆角修复**：打包测试镜像，确认"Type-C 断层"（端帽）已修、回收站功能正常。
> 还要求：先开工第 1 条（本次即按此顺序）。

## 1. 本批做成了什么

### 1.1 ★ 桌面 = 真实文件夹（新模块 `kernel/deskdir64.h/.cpp`，约 480 行，只进系统内核）

**真源换掉了**：桌面文件项不再是 `/etc/desktop.links` 里登记的"快捷方式"，而是**列目录列出来的条目**。
`.links` 退化成**位置缓存**（只供 x/y；格式仍是 v0.11.1 的 5 段 `dir|letter|x|y|path`，只有 path 落在
当前桌面目录里的行才算数，其余计进日志 `skipped=`）。既有验收对 `[DESK64] links load/save` 的断言**一字不改**。

- 目录 = GUI 用户的 `desktop` 字段（`userdb64_gui_user64()`）；root → `/root/Desktop`，普通用户 →
  `/home/<name>/Desktop`；未登录退化 `/home/vimtu/Desktop`。**登录换用户会自动 rebind**（`deskdir64_rebind64`）。
- `deskdir64_scan64()`：`vfs64_list64_on64` 逐条列（有界 512 条），名字以 `.` 开头的**不列**（计 `hidden=`），
  并与上次比较出 `added/removed/retyped`；只有"变了"或调用方明确要时才打日志（防 4Hz 刷屏）。
- `deskdir64_move_in64(src_vol, src_path, …)`：**真移动**。同卷 = `fs64_rename_to64`（一次改名，字节不动）；
  跨卷单文件 = `explorer64_copy_file64` + `fs64_unlink64`（同一套 64 KiB 分块复制）；**目录跨卷拒绝**
  （`why=crossvol-dir`，如实）；目标同名的自动退化成 `stem(2).ext`（99 次以内，**绝不覆盖**）。
- `deskdir64_trash64(name)`：桌面条目 → `/Recycle`（`trash64_put64`，同卷改名，不复制、不漏数据）。
- `deskdir64_move_out64 / rename64 / unique_name64 / selftest64`（自检 6 条纯逻辑判据，不碰盘）。

**实时刷新（帧内零 I/O 轮询）**：`kernel/vfs64.cpp` 新增 `vfs64_mut_gen64()` —— 每次**写类**操作
（write/write_at/write_stream/truncate/create/symlink/mkdir/unlink/rmdir/rename/rename_to/chmod/chown）
只做一次 `g_mut_gen++`（纯内存）。外壳 `desktopops64_tick64`（15 tick ≈ 0.25 s）**只比较这一个整数**，
变了才去重列桌面目录。于是"终端里 `touch` 一个文件 → 0.25 s 内桌面出图标"，平时一个字节的磁盘 I/O 都没有。
为什么不做成"每帧 stat 目录 mtime"：帧内 I/O 会与 ring3 写者抢 VFS 大锁，ATA 一旦回退轮询，单次读可达 1 s
（v0.11.1 的 `/etc/apps.json` 帧内轮询就是这么把鼠标键盘打死的）。

**外壳侧（`kernel/desktopops64.cpp`）**：
- `desk_sync64(why)`：摘掉全部 kind 9 → 按目录条目重建 → 位置优先级 ①上次同一路径 ②位置缓存 ③第一个空格；
  条目集合真变了才作废选择集合、才落一次缓存、才重打 `[DESK64] item` 行（含新增的 `glyph=`）。
- `desktopops64_add_file64()` 从"登记路径"改成"**调 deskdir64_move_in64 真搬**"，再定位新项做吸附/避让，
  并显式 `links_save64("add")`（既有验收按 `links save why=add n=1 … ok=1` 判真落盘）。
- `desktopops64_remove_file64()` = 真送进 `/Recycle`。
- `desktopops64_tick64()`：config 抽查 + 应用注册表世代号（既有）+ **GUI 用户 rebind** + **VFS 世代号重列**。

### 1.2 回收站合并成**一套数据**（`kernel/desktopops64.cpp` 回收站窗口）

- 桌面回收站窗口的行 = **虚拟项（内存 `g_recycle[]`）+ `/Recycle` 里的真实条目**（`trash64_list64`）。
  恢复 = 虚拟项写回 `ui.desktop.icons` / 真实条目走 `trash64_restore64`（不覆盖，`why=exists`）；
  永久删除 = `trash64_purge64`（有界递归）；清空 = `trash64_empty64`。**两段式确认照旧。**
- 打点保留既有格式（`[RECYCLE64] add/restore/delete ask/delete done/geom/btn/row`），
  新增 `[RECYCLE64] files why=… n=<真实条目> virt=<虚拟项>`（一眼看出两个来源各多少）。

### 1.3 图标按扩展名匹配（用户原话："txt 就是文本编辑器图标"）

`desk_file_glyph_kind64()` 分 5 档：**0 文件夹 / 1 文本 / 2 图片 / 3 可执行 / 4 其它**
（文本：`.txt .md .log .c .cpp .h .hpp .py .sh .json .cfg .ini .xml .html .css .js`；
图片：`.png .bmp .jpg .jpeg .gif .ico`；可执行：`.elf .vap .bin`）。自绘几何（与全系统同一套口径）：
文件夹=琥珀文件夹、文本=白纸 + 5 行偏蓝、图片=画框 + 太阳 + 山、可执行=实心三角。
**自家/第三方应用**仍走 `draw_app_shortcut_icon64`（包内 PNG / 系统图标名 / 程序化兜底），两者不相干。
图标档位进串口：`[DESK64] item idx=… kind=9 name=a.txt … glyph=1`（可 grep，不靠肉眼）。

### 1.4 MicroPython 贪吃蛇：**真跑起来了**（并修掉一个真 bug）

- `user/micropython/demo/snake.py` 的方向表写错了：`DIRS` 是 `(dy, dx)` 语义，却写成
  `{"w": (0,-1), "a": (-1,0), …}` —— 相当于整体转 90°，按 `w` 实际往左走，**第一步就咬到自己**
  （实测串口：`咬到自己了。最终分数 0（1 回合）`）。改成 `w=(-1,0) a=(0,-1) s=(1,0) d=(0,1)`。
- 真机截图（宿主侧 QEMU `screendump` → PNG）：`docs/screenshots/v0117_snake.png`（1280×800）。
  证据链：`[MPDRV] start path=/bin/mp argc=2`、`[TERM] micropython … script=/mpy/snake.py`、
  串口里连续 7 屏棋盘（分数 0，回合 0→6）；PNG 客户区内浅色文本像素 17,512 个（不是空屏）。

## 2. 验收（本轮实跑，逐条）

| 脚本 | 结果 | 说明 |
|---|---|---|
| `tests/deskfolder64_test.py`（**新增**，桌面=真实文件夹端到端） | **64/74** | 三块能力全绿：真目录绑定、终端写文件实时刷新、拖到桌面真移动、重启持久；2 处红见 §3 |
| `tests/dragdrop64_test.py`（跨层拖拽） | **51/51 PASS** | 判据按 v0.11.7 语义重写（见 §2.2） |
| `tests/desktopops64_test.py`（桌面交互/回收站/菜单/框选/持久化） | **72/72 PASS** | 回收站窗口改成"虚拟项 + 真实条目"后，既有判据一条没改、全绿 |
| `tests/version64_test.py`（发版第 1 条硬门禁） | **8/8 PASS** | 内核内嵌 `VimtuOS 0.11.7`、旧串 0 次、串口自报 `VimtuOS 0.11.7 x86_64` |
| 构建 | `EXIT=0` | `kernel64_os.bin` **3,518,416 B**（闸门 15,728,640）；ISO 93,458,432 B / `/VERSION` = `VimtuOS 0.11.7` |

### 2.1 deskfolder64_test 的绿的判据（实跑原文）

```
[DESKDIR] selftest PASS mask=63 dir=/home/vimtu/Desktop inited=0 scans=0
[DESKDIR] init dir=/home/vimtu/Desktop vol=0 exists=1 entries=0 total=0 mode=dir
[DESKDIR] scan why=fs dir=/home/vimtu/Desktop entries=1 total=1 added=1 removed=0 … names=live.txt   ← 只靠终端 touch 触发
[DESK64] item idx=… kind=9 name=live.txt … glyph=1                                                  ← .txt -> 文本档位
[DESKDIR] move in why=explorer-drag from=/home/vimtu/move_me.txt to=/home/vimtu/Desktop/move_me.txt vol=0 dir=0 via=rename rc=0 ok=1
[DESK64] file add idx=… kind=9 dir=0 vol=0 path=/home/vimtu/Desktop/move_me.txt … moved=1 why=explorer-drag
[DESKDIR] init dir=/home/vimtu/Desktop vol=0 exists=1 entries=4        ← 重启后目录里还是那 4 条
[DESKDIR] scan why=init … names=live.txt,docs2,pic.png,move_me.txt
```
（`glyph=0` 给 `docs2`、`glyph=2` 给 `pic.png` 也都 PASS —— 图标档位不是靠肉眼判的。）

### 2.2 dragdrop64_test 判据为什么重写（产品不改）

v0.11.1~v0.11.6：拖到桌面 = 在 `/etc/desktop.links` 里登记一条"快捷方式"，盘上什么都没搬，
所以拖 `/etc`（root 拥有）也能"成功"。v0.11.7 起是真移动：`/` 归 root(0:0) 0755，普通用户（uid=1000）
没有 w+x → `fs64_rename_to64` 被**权限正确拒绝**（`[DESKDIR] move in … rc=-1 ok=0 why=rename-failed`）。
于是阶段 3 的判据改成"**必须被拒绝**、且桌面项数一个都不许变"（`[DESK64] file add` 一条都不许有），
阶段 5（重启）改成"桌面上没有 kind=9 残留、桌面目录 entries=0"。
"拖到桌面能真搬过去"的**正向**用例在 `deskfolder64_test` 阶段 3（从 `/home/vimtu` 拖，绿）。

## 3. 未完成 / UNVERIFIED（点名，不冒充）

1. **"把桌面图标拖进回收站图标"这个手势，自动化没打中**（`deskfolder64_test` 阶段 4 红）。
   根因是**夹具**：QEMU 相对鼠标在 1280×800 下定位不稳（同一脚本里 x=24 的回收站图标能稳定命中，
   x=112/376 的文件图标 6 次 ±18 px 微调都命中不了，`[UI] desktop icon select kind=9` 一次都没出现；
   `park_cursor` 默认 58×20 = 1160 px < 屏幕宽 1280，已改成本文件的 `goto_abs()` 走 90 次）。
   产品侧链路是 `desktopops64_recycle_add64 → deskdir64_trash64 → trash64_put64`，
   其中 `trash64_put64` 的往返在 v0.11.6 的 `fileops64_test` 阶段 10/10.5 已端到端验过；
   **但"桌面图标 → 回收站"这一手势本轮没有被证明**。下游两条（`/Recycle` 行出现在桌面回收站窗口、
   恢复后桌面重新列出）因此连带红。下一轮建议：给桌面图标加键盘导航（或 QEMU 换 abs-pointer 设备）再验。
2. **窗口圆角像素判据**：`deskfolder64_test` 阶段 6 里"开窗截图 vs 最小化截图"的差分在本夹具下
   五点差分只有 1/1/2/10/8（判据要 ≥12）—— 说明两帧几乎相同（第二帧窗口没被最小化/或两帧都含窗口），
   **判据不成立**，标 UNVERIFIED。现有证据只有：`tests/deco64_test.py` 的**几何**判据 +
   `docs/screenshots/v0117_snake.png` 里肉眼可辨的窗口（顶部两角是弧、底边全宽直线）。
   下一轮：把"最小化"改成"关闭窗口"再截第二帧，或直接对 PNG 做行/列"落笔范围"分析。
3. `tests/fileops64_test.py`、`tests/micropython64_test.py` **本轮未复跑**（改动没触及 explorer 的删除路径、
   也没动 mp 端口配置；只改了 `snake.py` 内容）。下一轮补跑一次。
4. 远未做的（沿用旧单）：`圆角渲染bug.txt` 第二部分（开始菜单左上角时间块色差）、最大化时内容自适应（Layout）、
   桌面右键菜单的"重命名"仍是 `not implemented`（重命名走 explorer 可用）。
5. 边界（如实）：非空目录可删；**目录跨卷拖拽不支持**（`why=crossvol-dir`）；桌面双击文件 = 打开它所在目录并选中它
   （没有"按扩展名 open with"）；名字以 `.` 开头的条目不显示；虚拟项（我的电脑/回收站/终端）拖进回收站 =
   从 `ui.desktop.icons` 移除（没有文件，可恢复）；`ui.desktop.icons`（虚拟项）、`/etc/apps.json`（应用快捷方式）、
   桌面目录（真实文件）三个真源并列，各管一段。

## 4. 本批改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/deskdir64.h/.cpp` | **新增**：桌面目录的唯一读写入口（列目录/真移动/真回收站/唯一名/自检，`[DESKDIR]` 打点） |
| `kernel/vfs64.h/.cpp` | 新增 `vfs64_mut_gen64()` + 12 处写类入口 `mut_bump64()`（纯内存世代号） |
| `kernel/desktopops64.cpp` | `desk_sync64`（目录→桌面项）、`add_file64` 真移动、`remove_file64` 真进回收站、tick 接世代号+rebind、图标按扩展名分档、回收站行 = 虚拟项 + `/Recycle` |
| `kernel/desktopops64.h` | KIND_FILE 语义重写为"桌面上的真实条目"；新增 `desktopops64_desk_dir64()` |
| `kernel/kernel64.cpp` | 启动期 `deskdir64_selftest64()` |
| `user/micropython/demo/snake.py` | **修方向表 bug**（(dy,dx) 写反，`w` 变向左、一步咬自己） |
| `build64.sh` | 0.11.6→0.11.7；`SRCS_DESKTOP` 加 `deskdir64.cpp`；两条链接线加 `deskdir64.o` |
| `tests/deskfolder64_test.py` | **新增**：桌面=真实文件夹端到端（6 阶段，含像素判据） |
| `tests/dragdrop64_test.py` | 阶段 3/5 判据按"真移动 + 权限拒绝"重写 |
| `docs/screenshots/v0117_snake.png` | **新增**：贪吃蛇真跑截图（1280×800） |

## 5. 不要踩的坑（本批新增，都是实测）

1. **`vfs64_mkdirs64/mkdirs_core64` 只建父目录**（`cut` 停在最后一段之前）——想建 `/x/y/z` 得自己再
   `vfs64_mkdir_on64` 一次最后一段。`deskdir64_ensure64` 就是这么写的。
2. **`fs64_rename_to64(vol, old, new)` 要显式卷号**；`-1` 表示"当前卷"。跨卷 rename 在文件系统层面不成立。
3. **普通用户在 `/` 上什么都动不了**（root 0:0 0755）——所以"拖到桌面"的用例必须挑**用户自己的文件**
   （`/home/<user>/…` 或 `/tmp`），拿 `/etc` 当用例只会得到 `why=rename-failed`（产品对、用例错）。
4. **QEMU `sendkey` 不认大写字母**：`sendkey D` 被吞，路径 `/home/vimtu/Desktop` 变成 `/home/vimtu/esktop`
   → `touch` 直接失败（本轮第一版就栽在这）。敲命令必须自己映射大写 → `shift-<小写>`
   （`tests/deskfolder64_test.py` 的 `type_cmd()`）。
5. **双击 = `ex.double_click()`（5 个事件 / 0.1 s 间隔）**：`ex.single_click_at()` 自己 `sleep 0.55`，
   两次点之间 > 500 ms，GUI 不认双击（表现为只有 `[UI] desktop icon select`、没有 `open`）。
6. **`ex.park_cursor()` 默认 58×20 = 1160 px < 屏幕宽 1280**：从右侧出发时"归零"归不到 0，残留 ~100 px，
   之后所有 `blind_goto` 全偏。本文件用 `goto_abs()`（90 次）替代。
7. **桌面图标在窗口下面时，点击/拖拽命中的是窗口**：做桌面手势前先 `Ctrl+Shift+W` 最小化所有窗口。
8. **`desk_sync64` 不要每次都清选择集合**：只在"目录条目真变了"时清，否则终端里写个文件就把桌面选中项清掉。
9. **同一行日志里的字段顺序是有验收依赖的**：`[DESK64] file add … total=N why=<why>` 后面才能加新字段
   （`moved=1` 必须放在 `why=` 之后，否则 `dragdrop64_test` 的正则匹配不到）。

## 6. 现场（定稿）

- `kernel64_os.bin` **3,518,416 B**（闸门 15,728,640 B）；`build64.sh` 第 88 行 `VIMTUOS_VERSION="0.11.7"`
- 构建：`C:\msys64\usr\bin\bash.exe -lc "bash build64.sh"`（封装 `$env:PI_SCRATCH_DIR\run_build.sh`，日志 `build.log`，10~18 min）
- 语法自检：`$env:PI_SCRATCH_DIR\syncheck2.sh <文件…>`（clang `-fsyntax-only`，PATH 需含 `/mingw64/bin`）
- 关键打点（验收 grep 用）：
  `[DESKDIR] init|scan|move in|move out|rename|trash|selftest`、`[DESK64] item … glyph=`、
  `[DESK64] links load why=… bytes= n= skipped= ok= items= files= dir= path=`、
  `[RECYCLE64] files why=… n= virt=`、`[RECYCLE64] restore … real=1`
- 截图：`docs/screenshots/v0117_snake.png`（宿主侧脚本 `$env:PI_SCRATCH_DIR\snap_snake.py`）

## 7. 下一轮建议（按优先级）

1. 给桌面图标加**键盘导航**（Tab/方向键 + 回车删除/打开），顺手解决 §3.1 的夹具死角（也更好用）。
2. 圆角像素判据改成"关闭窗口前后两帧"或"落笔范围分析"（§3.2）。
3. 补跑 `fileops64_test` + `micropython64_test`；把 `deskfolder64_test` 的 74 条里那 10 条转绿。
4. 用户暂缓的发版流程：v0.11.6 + v0.11.7 两次都要走 `rel_*.py` + tag/Release + 附件回读
   （**旧 Release 一字不动**这一条照旧）。
