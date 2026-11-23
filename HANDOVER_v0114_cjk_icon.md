# HANDOVER —— v0.11.4 中文标题 + 真图标（内核当字形/图标唯一来源）

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.4"`（ISO 里 `/VERSION` 实测 `VimtuOS 0.11.4`）
- 状态：**功能完成、相关回归全绿；尚未发版**（tag / Release / 远端推送留给发布轮，见 §7）
- 一句话：v0.11.3 留下的两条"合成器只会画 ASCII 标题 / 只会画默认图标"的边界，本版从
  **根上**改掉了 —— 字形与图标的所有权收回到内核，Ring 3 合成器只做 alpha 混合。

## 1. 本版到底改了什么（三件事）

### 1.1 中文标题（31 号 `wl_surface_title_alpha`）
- **病灶**：`/lib/wm.elf` 的 ELF64 装载窗口只有 64 KiB（`kernel/usermode64.h`），`font64.o`
  一个目标文件 94,800 B —— 合成器**装不下** stb_truetype。v0.11.3 只能给它一张构建期预光栅化的
  ASCII 字模（`user/wm/wm_ttf.h`），于是"中文标题在 wm 路下不显示"（非 ASCII 按 8px 空白跳过）。
- **修法**：内核新增 `font_raster_text64()`（`kernel/font.cpp`）把 UTF-8 串用**内核自己的字形缓存**
  （四字体面 / 查询链 / stem 加厚）光栅化成 8-bit 覆盖率缓冲；31 号把它交给合成器：
  `wl_surface_title_alpha(surf, out, cap) -> (宽<<16)|高`，`cap >= 320*24 = 7680`，stride = 320。
- **合成器侧**（`user/wm/wm.c`）：`wm_blend8()`（与内核 `blend_px` 同式）+ `wm_title_text()`。
  31 号不可用时**如实回落** v0.11.3 的 ASCII 字模（降级路径保留）。
- **标题缓冲 32 → 64 B**（`kernel/wl64.h` `WL64_TITLE_MAX64`、`Wl64Surface.title`、
  `gui64.h Window.title`、`dock64 ext.title`、`wm.c WmSlot.title`）：中文 3 B/字，32 B 只够 10 字。
- 打点（验收按它钉）：`[WL64] title bmp surf=… w=… h=24 bytes=7680 utf8=… ink=… title="…"`
  —— **`ink` 是"真落了墨"的像素数**；只看 `w` 分不清"画了字"与"全是空白"（缺字也照样按 em 推进）。

### 1.2 标题栏真图标（32 号 `wl_surface_set_app` + 33 号 `wl_surface_app_icon`）
- 32 号给 surface 一个**应用身份名**（UTF-8，如 `image-viewer`），内核用
  `icons64_kind_by_name64()` 换成图标包 kind，存进 surface 表（`Wl64Surface.app_kind/app_name`）。
- 33 号把该图标按 16x16 RGBA 交给合成器（`icons64_export_rgba64`，颜色 = 品牌色）。
- **两条合成路同一把尺子**：`gui64_deco_frame64(x,y,w,h,title,active,icon_kind)`（内核窗口 + wl
  surface 共用）与 `wm.c wm_deco_frame` 都是：图标 16x16 @ (fx+6, fy+5)，有图标 → 标题文字左移到
  fx+26；`icon_kind = 0` 时文字仍在 fx+8（**旧的像素判据一个都不变**）。
- Dock 运行项图标：`dock64_ext_add64(surf, title, icon_kind)` + 新增 `dock64_ext_icon64()`
  （32 号调用时补登记）→ `[DOCK64] ext-add … icon=74`、`[DOCK64] ext-icon surf=… kind=74 name=image-viewer`。
- 图片查看器：`wl_surface_set_app(g_surf, "image-viewer")`，打点 `[VIEWER] app rc=0 name=image-viewer`。

### 1.3 资源管理器地址栏三个按钮 → 图标包 chevron（"纯箭头"退场）
- `kernel/explorer64.cpp draw_addr()`：原来的**纯像素三角**换成 `icons64_draw_kind64()` 的
  `chevron-left(13) / chevron-right(14) / chevron-up(21)`（同族 SVG，粗细/圆角/端点一致），
  取不到图标包时**如实回落**原像素三角。打点一行（一次）：
  `[EXP] nav icons pack=3 fallback=0 kinds=13,14,21 size=16`。
- 图标包新增两个 kind（`tools/make_iconpack.py` + `kernel/icons64.h` + `kernel/icons64.cpp` 名字表）：
  `21 chevron-up`、`74 image-viewer`（品牌色 `#8FD6FF`）。

## 2. 现场（下个会话必须核对）

| 项 | 值 |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,426,720 B**（闸门 15,728,640 B；余 12,301,920 B） |
| 安装内核 `build64/kernel64.bin` | 1,900,400 B |
| `build64/loader64.bin` | 4,090 B |
| `/lib/wm.elf` | **44,720 B**，PT_LOAD span **55,692 B**（装载窗口 65,536 B —— 还有 9.8 KB 空间） |
| `/bin/viewer`（`build64/viewer.elf`） | 118,856 B |
| 图标包 `build64/iconpack.bin` | **38,401 B（76 扇区）/ 32 个 kind / 96 个尺寸条目** |
| ISO / IMG / 磁盘镜像 | 93,435,904 / 42,661,888 / 38,467,072 B |
| 构建 | `bash build64.sh` → **EXIT=0**（必须 `C:\msys64\usr\bin\bash.exe`） |

## 3. 本版踩到并修掉的两个"预算/判据"坑（防复现）

1. **图标包把原始区顶爆了**：加两个新 kind（4+3 个尺寸档）让 `iconpack.bin` 49,192 → 51,893 B，
   而原始区是**硬预算**（1,799 扇区 = 921,088 B，v0.11.3 时只剩 184 B 余量）→
   `tools/demo_pack_win.py` 直接 `原始 blob 区 922,288 B 超过 … 921,088 B` **EXIT=1**，
   构建在 blob 打包处静默中止（`set -e` + 无 stderr 文本，极易误判成"构建卡住"）。
   **修法（不是加预算，是砍没人用的档）**：全工程系统图标的调用点只有 16/18/24
   （状态区 24、菜单 18、地址栏 16），48px 档从来没画过却占着预算 → `system` 只出 `(16,24,32)`，
   `apps` 仍保留 `(24,32,48)`（Dock 图标默认 46px，必须从 48 缩）。包回落到 **38,401 B**。
2. **8-bit 覆盖率让一条旧判据假红**：`tests/viewer64_test.py` 的"旧自绘标题栏 0x3A4658 = 0 px"
   用 `is_frame(tol=10)`；4-bit 字模换成 256 级覆盖率后，笔画边缘出现一档像素落进该邻域
   （实测 big.png 6 px）。旧画家是**整片实色填充**，tol=2 也照样能抓住 → 把该判据**收紧到 tol=2**
   （强度不降，理由写在代码注释里）。

## 4. 验收（本构建实跑，串行一条链）

| 脚本 | 结果 |
|---|---|
| `tests/viewer64_test.py` | **PASS 137/137**（含新增 12 条：内核 31 号 `utf8=1 ink>0`、wm 用内核位图 + icon=74、标题栏品牌蓝像素 ≥20） |
| `tests/docklife64_test.py` | **24/24 PASS** |
| `tests/wmdeco64_test.py` | **24/24 PASS** |
| `tests/deco64_test.py` | **14/14 PASS** |
| `tests/explorer64_test.py` | **PASS 91/91**（新增：`[EXP] nav icons pack=3 fallback=0 kinds=13,14,21`） |
| `tests/icons64_test.py` | **52 项 0 FAIL**（新增 `image-viewer` 品牌色进 `APP_COLOR`，与内核同值） |
| `tests/wm64_test.py` | **89/89 PASS** |
| `tests/version64_test.py` | **8/8 PASS**（`VimtuOS 0.11.4`，旧串 0 次） |
| 第二轮（装饰/图标像素密集面）的 6 支 | 见下面 §4.1（含两条先红后绿的如实定性） |

### 4.1 第二轮（装饰/图标的像素密集面）与两条"先红后绿"的如实记录

| 脚本 | 结果 |
|---|---|
| `tests/gui_modern64_test.py` | 首轮 **FAIL**（Dock 回弹 `el=404ms > 390ms` 预算）→ **复跑 PASS** |
| `tests/settings64_test.py` | **111/111 PASS**（标题栏图标 + 窗口像素全过） |
| `tests/startmenu64_test.py` | **63/63 PASS** |
| `tests/panels64_test.py` | 首轮 **FAIL**（日历打开动画只有一帧 `t=0 s=1000 dy=0 alpha=255`）→ 重建 pristine 镜像后 **78/78 PASS** |
| `tests/wmfull64_test.py` | **PASS 41 / FAIL 0 / SKIP(env) 0** |
| `tests/desktop64_test.py` | **PASS** |
| `tests/status_report.py` | **完成 110 / 部分 0 / 未做 0**（能力清单口径不变） |

两条失败的定性（都不是本版代码回归，但都是真问题，必须记住）：

1. **`gui_modern64_test` 的 Dock 回弹 `el<=390ms`**：`tests/status_report.py` 早就把它记成
   "既有窄竞态，靠复跑判定，不看单次结果"（本轮 404ms / 复跑 PASS）。动画时长由
   `theme64_dur64()` + 实际 tick 计数决定，QEMU TCG 下的抖动就能顶出预算。
2. **`panels64_test` 的日历动画**：根因是 **`build64/system.img` 被测试改脏了** ——
   卷里的配置残留 `cfg.ui.reduce_motion = 1`（"减少动画"开着，`theme64_dur64()` 一律返回 0ms，
   于是动画只有 1 帧、`t=0`）。`panels64_test` / `desktop64_test` / `gui_modern64_test` 都是
   **直接启动 `build64/system.img`（不是拷贝）**，所以它们会把设置写回共享镜像。
   **重建后 pristine 镜像上立刻 78/78 绿** —— 结论：这是测试夹具卫生问题，不是渲染回归。
   **下个会话注意**：跑这三支之前先确认 `build64/system.img` 是构建产物（`bash build64.sh` 重出，
   或至少确认卷里没有 `cfg.ui.reduce_motion`），否则动画类判据会假红。

## 5. 剩余边界（如实，一条都不许抹）

1. **只有"声明了应用身份"的窗口有标题图标**：`wl_surface_set_app`（32 号）是**可选**的；没声明的
   wl surface 仍是"无图标"（标题文字回到 fx+8）。内核窗口靠 `APP_ID_*` → 注册表图标名，天然有。
2. **应用身份名只有认识的那几个**：`icons64_kind_by_name64()` 的表 = 包里的 32 个名字。写
   `"viewer"` 或任意没注册的名字 → kind=0（没有图标，**不报错**，这是刻意的：身份是可选装饰）。
3. **图标只有 16px 槽**：标题栏/Dock 用 16px RGBA；title bar 不做 20/24px 档，也不做高 DPI 缩放。
4. **`ink` 只证明"有墨"**：它不区分"汉字画对了"还是"缺字占位框"——缺字时 `font_raster_text64`
   直接跳过（不画占位框），`ink` 会明显偏小；真要钉字形正确性得靠像素比对（本轮没做逐字比对）。
5. **中文标题只到"字体面覆盖的码点"**：`build/font_simhei.ttf` 是子集；没收录的字 → 缺字（无墨，
   但仍按 em 推进）。`[FONT64] glyph miss cp=0x…` 会如实打点。
6. **wm 仍是 64 KiB 装载窗口**：本版把"字体/图标"挡在窗外，天花板不变 —— 再加一块大资源
   （如完整 CJK 字模、图标的 48px 档）仍然装不进去。
7. **`[WM] title bmp` 打点只有前 6 条**（防刷屏）：验收按"出现且 ok=1 icon=74"抓，不按条数。
8. **Dock 外部窗口表上限仍是 8**（`DOCK_EXT_MAX`，v0.11.3 遗留，本版没动）。
9. **系统图标不再有 48px 档**（预算换来的）：将来真要做 48px 系统图标（如大号状态区），
   要么把原始区预算做大（会动 `memlayout64.h` / `ML64_STORE_LBA`，牵连安装器与多个测试），
   要么按需从卷里 `/icons` 逐文件覆盖（`icons64` 已支持 `src=vfs` 覆盖层）。
10. **布局弹性评估仍未做**（`docs/布局弹性评估.md` 不存在，v0.11.2 起遗留）。

## 6. 本版改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/font.cpp/.h` | **新增** `font_raster_text64()`（文字 → 8-bit 覆盖率缓冲；复用既有字形缓存 + stem 加厚） |
| `kernel/wl64.h` | 新增 31/32/33 常量与语义段、`WL64_TITLE_BMP_W/H`、`WL64_APP_ICON_PX`、`WL64_TITLE_MAX64` |
| `kernel/wl64.cpp` | `title[64]`、`app_kind/app_name`、31（含 ink 打点）/32/33 实现、装饰调用带 icon_kind、Dock 登记带 kind |
| `kernel/syscall64.cpp` | 31/32/33 三个 case + weak 声明（安装介质内核 deny，不假装成功） |
| `kernel/gui64.h/.cpp` | `gui64_deco_frame64(..., icon_kind)`、标题栏图标绘制、`dock64_ext_icon64()`、Dock 项/运行项带 icon kind、标题缓冲 64 |
| `kernel/dock64.h` | `dock64_ext_add64(surf, title, icon_kind)`、`dock64_ext_icon64()` |
| `kernel/explorer64.cpp` | 地址栏三按钮换图标包 chevron（回落像素三角）+ `[EXP] nav icons` 打点 |
| `kernel/icons64.h/.cpp` | kind 21 `chevron-up` / 74 `image-viewer`（名字表 + 品牌色 `#8FD6FF`） |
| `tools/make_iconpack.py` | KINDS 整表重写：system `(16,24,32)`、apps `(24,32,48)` + 两个新 kind |
| `user/lib/wl.h/.c` | `wl_surface_title_alpha()` / `wl_surface_set_app()` / `wl_surface_app_icon()` |
| `user/wm/wm.c` | `wm_blend8()` / `wm_title_text()` / `wm_title_icon()`、标题带中文 + 图标、`[WM] title bmp` 打点 |
| `user/viewer/viewer.c` | 标题改中文（`图片查看器 <文件名> <W>x<H>`）+ `wl_surface_set_app("image-viewer")` |
| `tests/viewer64_test.py` | 新增 12 条（内核位图 ink / wm 取用 + icon kind / 品牌蓝像素）；旧画家判据收紧到 tol=2 |
| `tests/explorer64_test.py` | 新增 1 条（`[EXP] nav icons pack=3 fallback=0`） |
| `tests/icons64_test.py` | `APP_COLOR` 增 `image-viewer`（与内核同值） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.3 → 0.11.4` |

## 7. 还没做（下一轮，按序建议）

1. **发版**：`VIMTUOS_VERSION` 已改、产物已出（ISO 93,435,904 / IMG 42,661,888）；tag + Release 流程见
   `docs/发布流程.md` 与本仓库既有 `$env:PI_SCRATCH_DIR\rel_v0113.py`（改成 0.11.4 即可）。
   **判据**：旧 Release 附件 id/size/digest/updated_at 逐条不变。
2. **全量清扫**：`py -3 tests/status_report.py --full` 一条链跑完（本版只跑了相关 14 个脚本）。
   注意 `multivol64_test.py` 在 900 s 预算下**曾超时**（v0.11.3 那轮清扫到它时留下孤儿 QEMU），
   单跑与清扫都要看 `Get-Process qemu-system-x86_64` 有没有残留。
3. **图标身份补齐**：PE 窗口（`user/win/peload_crt.c`）也调 32 号声明身份；把 `DOCK_EXT_MAX` 8 → 与
   `WL64_MAX_SURFACES64` 对齐。
4. **中文标题的逐字像素校验**（今日只有 `ink>0`）：拿宿主侧 PIL 渲染同一 em 的对照图做逐像素比对。
5. **系统图标 48px 档**：若界面真需要，走"卷里 `/icons/<sub>/<name>@48.png` 逐文件覆盖"（不动原始区预算）。
6. **文件管理器交互收口**（用户提的第 2 项）：拖拽 / 右键新建 / 删除 / 重命名在界面层逐条像素+串口双判据。
7. **MicroPython 小工具**（用户提的第 3 项）：`build64/micropython.bin` 已在产物里，先做
   "终端里跑一个脚本"的端到端验收。

## 8. 不要踩的坑（本版新增）

1. **原始区是硬预算**：`iconpack.bin` / 演示 blob / 中文字体共用 921,088 B。加图标 kind 前先看
   `tools/demo_pack_win.py` 的 `DEMO64_RAW_MAX_BYTES`（或直接跑一次 `--raw/--header` 探针）。
   超限时它 **EXIT=1 但没有 stderr 文本**（只有一行中文），`set -e` 会让整条 build 静默停在 blob 阶段。
2. **别在 `python` 侧用 `-Encoding UTF8` 读日志后判断空**：PowerShell 5.1 的 `Get-Content -Tail` 对
   无 BOM 的 UTF-8 日志可能给空 —— 用 `Where-Object { $_ -match '\S' } | Select-Object -Last N`。
3. **图标名字表三处必须同步**：`tools/make_iconpack.py` 的 KINDS、`kernel/icons64.cpp` 的
   `kIcon64Names`、`kernel/icons64.h` 的 kind 宏。少一处 = `icons64_test` 会红在"每个 kind 都有 load"
   或"load 的 path/尺寸"那两条上。
4. **测试里的品牌色表**（`tests/icons64_test.py` 的 `APP_COLOR`）也要同步 —— 否则它按"系统图标"
   的尺寸口径去算 `want`，会误判成 `@48` 不对。
5. **`[WL64] title bmp` 的字段顺序别改**：`surf= pid= w= h= bytes= utf8= ink= title="…"`，
   `viewer64_test` 按这一串抓。

## 9. 留档（scratch）

- `t_v0114_chain.log` / `t_v0114_chain2.log`：两轮串行回归的 rc 与末行
- `t_viewer64_test.log`、`t_docklife64_test.log`、`t_wmdeco64_test.log`、`t_deco64_test.log`、
  `t_explorer_retry.log`、`t_icons64_retry.log`、`t_wm64_test.log`、`t_version64_test.log`
- `run_tests_v0114.ps1` / `run_tests_v0114_b.ps1`：串行链脚本（`cmd /c` 重定向 + 退出码）
- `build.log`：本版构建日志（含 `Build OK … 系统内核 = 3426720 bytes`、ISO/IMG 尺寸）
- `full_sweep_0113.partial.log`：v0.11.3 那轮**被主动停掉**的全量清扫留档（只到能力清单，
  因为 `status_report.py --full` 是"全部跑完才打印"，缓冲里没有测试结论；**不能当结论用**）
