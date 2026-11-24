# HANDOVER —— v0.11.5 **已发布**（中文标题 + 真图标 + 文件管理器交互收口；封存单）

- 日期：2026-11-24（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.5"`
- 分支/HEAD：`main` —— **功能基线 = `2044d59d`（tag `v0.11.5` 指向它，Release 也挂在它上面）**
- tag：**`v0.11.5`**（annotated，tag 对象 `265c8e0a496fc508ecbe788fcd77aac9b79a9974` → commit `2044d59d`）
- Release：**已发布** id **`456278688`**，`draft=false / prerelease=false`，附件 2 个
  - `vimtu64-64.iso` 93,435,904 B sha256 `3d78f8b08fb5c…`（完整串见 Release 正文）
  - `vimtu64-64.img` 42,661,888 B sha256 `a6b1dd718741e…`
  - **旧版 v0.11.3 的两个附件 id/size/digest/updated_at 逐条未变**（脚本回读核对 PASS：
    img id `782074361` size 42,663,936 updated `2026-11-23T08:02:11Z`；
    iso id `782073579` size 93,437,952 updated `2026-11-23T08:02:00Z`）
- 远端：`main` == 本地 `2044d59d`；tag 远端对象 sha 与本地**逐位相同**（`265c8e0a…`）

> 本版是 **v0.11.4 + v0.11.5 两批功能合并发布**（v0.11.4 当时只构建、未发版）。

## 1. 现场（下个会话必须核对）

| 项 | 值 |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,431,616 B**（闸门 15,728,640 B） |
| 安装内核 `build64/kernel64.bin` | 1,900,400 B |
| `build64/loader64.bin` | 4,090 B |
| `/lib/wm.elf`（Ring 3 合成器） | **44,720 B**，PT_LOAD span **55,692 B**（装载窗口 64 KiB） |
| `/bin/viewer`（`build64/viewer.elf`） | 118,856 B |
| 图标包 `build64/iconpack.bin` | **38,401 B（76 扇区）/ 32 kind / 96 条目** |
| ISO / IMG / 磁盘镜像 | 93,435,904 / 42,661,888 / 38,467,072 B |
| 构建 | `bash build64.sh` → **EXIT=0**（必须 `C:\msys64\usr\bin\bash.exe`） |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = 158（`*_test.py` 149）；`TESTS` = 106 条登记 |

## 2. 本版做成了什么（三块）

### 2.1 中文窗口标题（31 号 `wl_surface_title_alpha`）
- 病灶：`/lib/wm.elf` 的 ELF64 装载窗口只有 64 KiB，`font64.o` 94,800 B —— 合成器**装不下**
  stb_truetype，v0.11.3 只能给它构建期预光栅化的 **ASCII** 字模 → 中文标题在 wm 路下不显示。
- 修法：内核 `font_raster_text64()`（复用四字体面缓存 + stem 加厚）把 UTF-8 串光栅化成 8-bit
  覆盖率位图；31 号交给合成器（320x24，stride=320，cap≥7,680）。wm 侧 `wm_blend8()` 与内核
  `blend_px` 同式。**两条合成路的标题像素来自同一套字形缓存。**
- 标题缓冲 32 → 64 B（中文 3 B/字）。打点 `[WL64] title bmp … utf8=1 ink=<真落墨像素数>`。

### 2.2 窗口 / 任务栏真图标（32 号 `wl_surface_set_app` + 33 号 `wl_surface_app_icon`）
- 32 号：应用身份名（`image-viewer` 等）→ `icons64_kind_by_name64()` → surface 表 `app_kind`。
- 33 号：把图标按 16x16 RGBA（品牌色 + 覆盖率 alpha）交给合成器。
- 标题栏：图标 16x16 @ (fx+6, fy+5)，有图标则标题文字左移 fx+26；`icon_kind=0` 时像素与 v0.11.3 一致。
- Dock 运行项（外部窗口）：`dock64_ext_add64(...icon_kind)` + `dock64_ext_icon64()`，
  画品牌色真图标；`[DOCK64] ext-add … icon=74` / `[DOCK64] ext-icon … kind=74 name=image-viewer`。
- 资源管理器地址栏三按钮：像素三角 → 图标包 `chevron-left/right/up`（同族 SVG），
  `[EXP] nav icons pack=3 fallback=0 kinds=13,14,21`。
- 图标包新增 kind **21 `chevron-up`** / **74 `image-viewer`**；为守住原始区硬预算，砍掉从未被绘制的
  系统图标 48px 档（包 38,401 B / 32 kind / 96 条目）。

### 2.3 文件管理器交互（拖拽进文件夹 = 真移动 + 判据按实现重写）
- `exp_drag_finish()` 三态结算：`target=dir` → `fs64_rename_to64()` 同卷跨目录改名；
  `target=desktop` → 老语义（desktopops64 一个像素不改）；`target=none` → 什么都不做。
- 拒绝规则：`why=readonly`（FAT32）/ `why=self`（目录拖进自己子孙，防环）/ `why=same` / `why=rc`（撞名）。
  撞名**不**偷偷改 `(2)`（与 F2 重命名同口径）。
- `fileops64_test` 三条过期判据（v0.11.1 起实现早换了）按实现重写：空白菜单 **items=5**、
  亚克力圆角弹窗（浅色灰调 + 四角圆角 + 1px 白高光边 + 有文字）、属性面板同一套；
  并**补上"右键 → 新建文本文件"**的验收（`[UI] explorer mkfile … rc=0` + item 行 + 终端 `ls`）。

## 3. 验收（本构建实跑，一条链串行）

| 脚本 | 结果 |
|---|---|
| `tests/viewer64_test.py` | **137/137 PASS** |
| `tests/docklife64_test.py` | **26/26 PASS**（含 B2：Dock 真图标 1,882 品牌蓝像素 + `ext-icon kind=74`） |
| `tests/wmdeco64_test.py` | **24/24 PASS** |
| `tests/deco64_test.py` | **14/14 PASS** |
| `tests/explorer64_test.py` | **91/91 PASS** |
| `tests/icons64_test.py` | **52/52 PASS** |
| `tests/wm64_test.py` | **89/89 PASS** |
| `tests/settings64_test.py` | **111/111 PASS** |
| `tests/startmenu64_test.py` | **63/63 PASS** |
| `tests/panels64_test.py` | **78/78 PASS** |
| `tests/wmfull64_test.py` | **PASS 41 / FAIL 0 / SKIP(env) 0** |
| `tests/desktop64_test.py` | **PASS** |
| `tests/fileops64_test.py` | **100/100 PASS** |
| `tests/dragdrop64_test.py` | **53/53 PASS** |
| `tests/version64_test.py` | **8/8 PASS**（串口 `VimtuOS 0.11.5 x86_64`；旧串 v0.11.3 在内核里 **0 次**） |
| `tests/status_report.py` | **完成 110 / 部分 0 / 未做 0** |

## 4. 剩余边界（如实，**一条都不许抹**）

### 4.1 删除 / 文件操作
1. **删除不进回收站**：两段式确认（10 秒内按两次）后直接 `fs64_unlink64`，**没有 `/Recycle`**，
   **没有恢复/永久删除**语义。
2. **非空目录不能删**：如实拒绝并提示（"目录非空，暂不支持递归删除"），没有递归删除。
3. **拖拽只做同卷移动**：跨卷拖动不做（那是 copy+delete）；跨卷请用 Ctrl+X / Ctrl+V。
4. **拖拽落点只有两种**：桌面空白 / 内容区的目录条目（面包屑段、导航窗格、Dock 都不是落点）。
5. **撞名不自动改名**（拖拽/重命名）：直接拒绝；自动 `(2)` 只发生在粘贴与新建。

### 4.2 标题 / 字体
6. **中文标题只覆盖字体面子集**：`build/font_simhei.ttf` 是子集，未收录的码点画不出来
   （`[FONT64] glyph miss cp=0x…` 如实打点）。
7. **应用身份（32 号）是可选**：没声明的窗口没有标题图标（文字回到 fx+8）。
8. **图标名只认包里的 32 个名字**：写别的名字 → kind=0（没有图标，不报错）。
9. **标题栏图标只有 16px 槽**，不做高 DPI 缩放。
10. **`ink` 只证明"有墨"**：不区分"汉字画对"还是别的字形，逐字像素比对仍未做。

### 4.3 缩放 / 最大化
11. **最大化按比例缩放是最近邻**（放大后是像素块，不做双线性）。
12. **最大化不改 surface 尺寸**：多余两边是 letterbox 的客户区底色。
13. 查看器自己的滚轮缩放也是最近邻 + **裁剪视图**（上限 4000‰、下限 = 适配画布）。

### 4.4 Dock / 窗口生命周期
14. **Dock 外部窗口表上限 8**（`DOCK_EXT_MAX`），超出不上任务栏。
15. **"活动窗口" = 最高 id 且未最小化**，没有真正的焦点栈。
16. **最小化不改几何**（`restore` 靠"几何没动过"逐像素还原）。
17. **`30` 号 ctl 对内核上下文（pid≤0）放行**（Dock 点击在 ring0 路径）—— 权限模型的既有放松。
18. **`/bin/wm` 仍是 64 KiB 装载窗口**：本版把字体/图标挡在窗外，天花板不变。
19. **`[WM] title bmp` 打点只有前 6 条**（防刷屏），验收按"出现且 ok=1 icon=74"抓，不按条数。

### 4.5 图标包预算
20. **系统图标没有 48px 档**（原始区 1,799 扇区 = 921,088 B 是硬预算；v0.11.3 时只剩 184 B 余量）。
    将来真要 48px 系统图标，要么改 `memlayout64.h`/`ML64_STORE_LBA`（牵连安装器与多个测试），
    要么走卷里 `/icons/<sub>/<name>@48.png` 逐文件覆盖（`icons64` 已支持该覆盖层，但**未测**）。

### 4.6 其它遗留
21. **布局弹性评估仍未做**：`docs/布局弹性评估.md` 不存在（v0.11.2 起遗留）。
22. **`multivol64_test.py` 在 900 s 预算下曾超时**（v0.11.3 那轮清扫留过孤儿 QEMU）；
    **全量 `status_report.py --full` 本轮仍未跑完**（只跑了上面 15 支相关脚本）。
23. **`build64/system.img` 会被 `panels64_test` / `desktop64_test` / `gui_modern64_test` 直接启动并写脏**
    （实测残留 `cfg.ui.reduce_motion=1` 会让动画类判据假红）—— 跑这三支前先重建镜像。

## 5. 本版改动的文件（提交 `2044d59d` 及其前置）

| 文件 | 要点 |
|---|---|
| `kernel/font.cpp/.h` | **新增** `font_raster_text64()`（文字 → 8-bit 覆盖率缓冲，复用字形缓存 + stem 加厚） |
| `kernel/wl64.h/.cpp` | 31/32/33 常量与语义段；`title[64]`；`app_kind/app_name`；31（含 `ink` 打点）/32/33 实现；装饰带 icon_kind；Dock 登记带 kind |
| `kernel/syscall64.cpp` | 31/32/33 三个 case + weak 声明（安装介质内核 deny，不假装成功） |
| `kernel/gui64.h/.cpp` | `gui64_deco_frame64(..., icon_kind)` + 标题栏图标；`dock64_ext_icon64()`；Dock 项/运行项带 icon kind；标题缓冲 64 |
| `kernel/dock64.h` | `dock64_ext_add64(surf, title, icon_kind)`、`dock64_ext_icon64()` |
| `kernel/explorer64.cpp` | 地址栏三按钮换 chevron + `[EXP] nav icons` 打点；`exp_drop_dir64()`；`exp_drag_finish()` 三态结算 + `[UI] explorer drag move` |
| `kernel/icons64.h/.cpp` | kind 21 `chevron-up` / 74 `image-viewer`（名字表 + 品牌色 `#8FD6FF`） |
| `tools/make_iconpack.py` | KINDS 整表重写：system `(16,24,32)`、apps `(24,32,48)` + 两个新 kind |
| `user/lib/wl.h/.c` | `wl_surface_title_alpha()` / `wl_surface_set_app()` / `wl_surface_app_icon()` |
| `user/wm/wm.c` | `wm_blend8/wm_title_text/wm_title_icon`；标题带中文 + 图标；`[WM] title bmp` 打点 |
| `user/viewer/viewer.c` | 标题改中文（`图片查看器 <文件名> <W>x<H>`）+ `wl_surface_set_app("image-viewer")` |
| `tests/viewer64_test.py` | +12 条（内核位图 `ink` / wm 取用 + icon kind / 品牌蓝像素）；旧画家判据收紧到 tol=2 |
| `tests/docklife64_test.py` | +2 条（B2：Dock 真图标品牌蓝像素 + `ext-icon kind=74`） |
| `tests/explorer64_test.py` | +1 条（`[EXP] nav icons pack=3 fallback=0`） |
| `tests/icons64_test.py` | `APP_COLOR` 增 `image-viewer`；`status_report` 的 icons64 判据改为 kinds≥32 且 entries≥kinds*3 |
| `tests/dragdrop64_test.py` | +17 条（/tmp 造素材 → 右键刷新 → 拖进文件夹 → 四条判据 + 两个反例） |
| `tests/fileops64_test.py` | 三条过期判据按实现重写 + 新增阶段 11.5「右键新建文本文件」 |
| `build64.sh` | `0.11.3 → 0.11.4 → 0.11.5` |
| `README.md` / `docs/发布流程.md` / `docs/项目状态总览.md` | 版本行与"最新发布"段回写 |

## 6. 不要重做 / 不要踩的坑（本版新增）

1. **原始区是硬预算**：`iconpack.bin` / 演示 blob / 中文字体共用 921,088 B。加图标 kind 前先跑一次
   `py -3 tools/demo_pack_win.py --build build64 --raw <tmp> --header <tmp>` 探针；超限时它 **EXIT=1
   但没有 stderr 文本**，`set -e` 会让整条 build 静默停在 blob 阶段。
2. **图标名字表三处必须同步**：`tools/make_iconpack.py` KINDS、`kernel/icons64.cpp kIcon64Names`、
   `kernel/icons64.h` 的 kind 宏；`tests/icons64_test.py` 的 `APP_COLOR` 也要同步。
3. **8-bit 覆盖率会让旧颜色判据假红**：viewer64 的"旧自绘标题栏 0x3A4658 = 0 px"必须 tol=2。
4. **亚克力弹窗没法用"某个固定 RGB"判**：菜单/属性面板从 v0.11.1 起是 `p2ui_popup64`；
   浅色主题下板面本身就接近纯白（实测 `pure_white=17301/23680`），**不能**用"纯白像素少"区分新旧。
5. **前台窗口决定点击落在谁身上**：终端造完素材后必须先 `ensure_window_on_top` 再双击/右键，
   否则点击落在终端上（实测双击无反应、右键不弹菜单、`ls` 输出跑进开始菜单）。
6. **普通用户在 `/` 建不了文件**（`[PERM64] deny op=create … need=wx`）—— 交互验收要在 `/tmp` 里做。
7. **`git push` 仍时通时不通**：本轮 `main` 由 **Git Data API 逐提交回放**（5 个提交全部 sha MATCH、
   递归树回读 14,696 项 bad=0）；tag 也由 API 建（tag 对象 sha 与本地逐位相同）。
   **注意**：旧的 `push_api_v0113.py` **只支持"领先 1 个提交"**；多提交请用
   `$env:PI_SCRATCH_DIR\push_api_chain_v0115.py`（逐提交回放，每个都核 sha）。
8. **tag 对象的 message 结尾换行不能剥**：`git cat-file tag` 的正文带结尾 `\n`，
   剥掉后 API 算出的 tag sha 必然不同（本轮实测 `c464fab…` vs `265c8e0…`）。

## 7. 下一轮建议（按序）

1. **回收站**（删除的第二条真实语义）：`/Recycle` + 恢复/永久删除 + 二次确认。
2. **递归删除非空目录**（vfs64 侧有界递归 + 失败回滚）。
3. **MicroPython 小工具**（用户提的第 3 项，仍未做）：`build64/micropython.bin` 已在产物里，
   先做"终端里跑一个脚本"的端到端验收。
4. **拖拽落点扩展**：面包屑段 / 导航窗格 / Dock。
5. **全量清扫**：`py -3 tests/status_report.py --full` 一条链跑完（先重建 `build64/system.img`）。
6. **布局弹性评估**：`docs/布局弹性评估.md` + 设置页 min-max 试点。

## 8. 发布脚本留档（scratch）

- `$env:PI_SCRATCH_DIR\rel_v0115.py` —— 快照旧版附件 → tag 校验 → 建 Release + 上传 → 双向核对（**RESULT=PASS**）
- `tag_api_v0115.py` —— 本地 annotated tag + 远端 tag 对象（Git Data API，sha 逐位相同）
- `push_api_chain_v0115.py` —— **多提交**逐位回放（本轮 5 个提交全 MATCH，树回读 bad=0）
- `body_v0115.md` —— Release 正文；`v0113_assets.json` —— 旧版附件快照（id/size/digest/updated_at）
- `t_*.log` —— 本版各脚本实跑日志（viewer/docklife/fileops/dragdrop/explorer/…）
