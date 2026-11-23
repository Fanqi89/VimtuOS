# HANDOVER —— v0.11.5 文件管理器交互收口（拖拽进文件夹 = 真移动；右键新建/删除/重命名的判据按实现重写）

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.5"`
- 状态：**功能完成、两支交互验收全绿；未发版**
- 上一单：[HANDOVER_v0114_cjk_icon.md](HANDOVER_v0114_cjk_icon.md)（中文标题 + 真图标）

## 1. 这一轮到底补了什么

### 1.1 拖拽：落点从"只有桌面"扩成"桌面 / 文件夹"（真移动）
- **病灶**：v0.11.1 的拖拽只认 `exp_desktop_target()` 这一个落点 —— 拖到列表里的文件夹上是
  `target=none`。文件管理器最基本的一件事"把文件拖着挪进子目录"**根本没有**。
- **修法**（`kernel/explorer64.cpp`）：
  - `exp_drop_dir64(w,mx,my,&idx)`：落点是不是**本窗口里的一个目录条目**（拖到自己身上不算）。
  - `exp_drag_finish()` 三态结算：`target=dir` → `fs64_rename_to64(-1, from, to)` 同卷跨目录改名；
    `target=desktop` → 老语义（desktopops64，一个像素不改）；`target=none` → 什么都不做。
  - 拒绝规则（写死）：`why=readonly`（FAT32 只读卷）/ `why=self`（目录拖进自己的子孙，防环）/
    `why=same`（同名同路径）/ `why=rc`（撞名或卷错误）。成功才 `exp_refresh_dir()` + 提示。
  - 幽灵底板（`g_drag_drop`）在鼠标移动时也算"文件夹可放"，描边跟着亮。
- 打点（验收按它钉）：
  ```
  [UI] explorer drag move name=<文件> dir=<0|1> from=<老路径> to=<新路径> rc=<0|负> why=<标签> ok=<0|1>
  [UI] explorer drag drop path=<老路径> dir=<0|1> x=.. y=.. target=<dir|desktop|none> ok=<0|1>
  ```

### 1.2 fileops64_test 的三条判据过期（v0.11.1 起实现早就换了）
跑旧 `tests/fileops64_test.py` 得到 95 条里 3 条红，**全是判据过期，不是回归**：
1. `ctxmenu items=4 at=blank` → 实现是 **5 项**（v0.11.1 空白菜单加了"新建文本文件"）。
2. `菜单是白底面板（255）+ 边框(130,130,130)` → v0.11.1 起是 `p2ui_popup64` 亚克力圆角弹窗。
3. `属性面板底色 252,252,252 精确匹配` → 同一套亚克力底。
**修法（收紧而不是放宽）**：
- 空白菜单判据 = `items=5 at=blank`；
- 菜单：①浅色灰调（均值亮度 ≥ 210、灰度差 ≤ 12）②**四角圆角**（角像素与内部均值差 > 10）
  ③**1px 白高光边**（边环近白像素 ≥ 周长一半）④有文字（暗像素 > 100）；
- 关菜单（Esc）：同一矩形里"彩色条目内容"回来（实测 `sat 0 -> 841`）——
  **不是**用"白边消失"：浅色主题下菜单下面的内容卡本身也接近白（实测 ring 643→596 不可分辨）；
- 属性面板：浅色灰调亚克力（亮度 ≥ 210、灰度差 ≤ 14）+ 有文字。

### 1.3 补上"右键 → 新建文本文件"的验收（v0.11.1 加了这一项，一直没人钉）
`fileops64_test.py` 阶段 11.5：右键空白（画布空白处）→ 点第 2 项 → 打 `newfile.txt` → 回车。
判据三层：`[UI] explorer mkfile path=/newfile.txt rc=0`（真落盘）+ `item name=newfile.txt type=file`
（列表出现）+ 终端 `ls` 也看得到（vfs64 目录枚举一致）。

## 2. 现场（下个会话必须核对）

| 项 | 值 |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,431,616 B**（闸门 15,728,640 B） |
| 安装内核 `build64/kernel64.bin` | 1,900,400 B |
| `/lib/wm.elf` | 44,720 B（PT_LOAD span 55,692 B / 窗口 65,536 B） |
| 图标包 `build64/iconpack.bin` | 38,401 B（76 扇区）/ 32 kind / 96 条目 |
| 磁盘镜像 `build64/sysdisk.img` | 38,467,072 B |
| 构建 | `bash build64.sh` → **EXIT=0**（必须 `C:\msys64\usr\bin\bash.exe`） |

## 3. 验收（本构建实跑）

| 脚本 | 结果 |
|---|---|
| `tests/dragdrop64_test.py` | **53/53 PASS**（原 36 条 + 新增 17 条：拖进文件夹真移动 / 右键刷新 / 反例） |
| `tests/fileops64_test.py` | **PASS 100/100**（原 95 条判据按实现重写 + 新增"新建文本文件"） |
| `tests/explorer64_test.py` | **PASS 91/91**（文件管理器主验收：本版改了拖拽结算路径，逐条复跑确认无连带） |

### 3.1 本轮踩到的两条"环境/注入"坑（不是产品缺陷，但会让人误判）

1. **普通用户在根目录建不了文件**：夹具盘 `/` 归 `root(0:0) 0755`，uid=1000 的终端 `write /x`
   会被 `[PERM64] deny op=create … need=wx` 挡下。所以"文件操作交互验收"必须落在**用户可写目录**
   （`/tmp`）。dragdrop64 新增那一节就是先在 `/tmp` 造素材，再双击 `tmp` 进目录做拖拽。
2. **前台窗口决定点击落在谁身上**：终端造完素材后如果不把文件管理器点回最前，
   双击/右键全落在终端上（实测：双击 `tmp` 毫无反应、右键不弹菜单、`ls` 的输出跑进开始菜单变成
   `entries=-1`）。顺序必须是：造素材 → `ensure_window_on_top` → 双击进目录 → 右键。
3. `fileops64_test` 首轮曾因 `exp.calibrate()` 返回 None 红一条（"鼠标位移模型"），**复跑 100/100** ——
   与本仓库既有的 PS/2 注入抖动同一类，判据不改。

## 4. 剩余边界（如实）

1. **拖拽只做同卷移动**：跨卷拖动不做（那不是 move 而是 copy+delete）；跨卷请用 Ctrl+X / Ctrl+V。
2. **撞名不自动改名**：拖进文件夹撞名直接拒绝（`why=rc` + 提示），不偷偷改成 `(2)`
   —— 与 F2 重命名同口径；粘贴/新建才是自动 `(2)` 策略。
3. **没有拖拽到"面包屑/导航窗格"的落点**：只认内容区里的目录条目。
4. **删除仍是两段式（10 秒内按两次）+ 非空目录拒绝**：没有回收站、没有递归删除（v0.11.2 起遗留）。
5. **重命名/F2 的行为本轮没动**（fileops64 的旧判据仍然覆盖它并全绿）。
6. **`DOCK_EXT_MAX = 8`、`/icons` 逐文件覆盖通道未测**（v0.11.3/0.11.4 遗留）。

## 5. 本版改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/explorer64.cpp` | `exp_drop_dir64()/exp_streq64()/exp_starts_with64()`；`exp_drag_finish()` 三态结算（dir/desktop/none）+ `[UI] explorer drag move` 打点；`exp_tick`/`exp_drag_begin` 的 `g_drag_drop` 也算文件夹落点 |
| `tests/dragdrop64_test.py` | 阶段 4.5：/tmp 造素材 → 右键刷新 → 拖进文件夹 → 四条判据 + 反例；`right_click_at()/left_click_at()/last_items()` 三个助手 |
| `tests/fileops64_test.py` | 三条过期判据按实现重写（亚克力/圆角/高光边/items=5）；新增阶段 11.5「右键新建文本文件」；`region_stats()/ring_light()` |
| `build64.sh` | `VIMTUOS_VERSION 0.11.4 → 0.11.5` |

## 6. 下一轮建议

1. **回收站**（删除的第二条真实语义）：`/Recycle` + 恢复/永久删除 + 二次确认（v0.11.2 起遗留）。
2. **递归删除**（非空目录）：目前如实拒绝；要做就在 vfs64 侧做有界递归 + 目录项回滚。
3. **拖拽落点扩展**：面包屑段 / 导航窗格 / Dock。
4. **MicroPython 小工具**（用户提的第 3 项，仍未做）：`build64/micropython.bin` 已在产物里，
   先做"终端里跑一个脚本"的端到端验收。
5. **全量清扫**：`py -3 tests/status_report.py --full` 一条链（注意 `multivol64_test.py` 的 900s 预算
   与 `build64/system.img` 会被 panels/desktop/gui_modern 直接启动并写脏）。
