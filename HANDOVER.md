# HANDOVER —— 交接单索引与协议（开工前必读）

> **给任何会话/任何 agent 的第一条规矩**：动这个仓库之前，先读本文件 → 再读下面"最新"那一份交接单 →
> 用 3 条命令核对现场（见 §2）。**不要凭印象开工**，本仓库的文档数字漂移是常态
> （例：README 写 95 项能力 / 77 个脚本，2026-11-22 实测是 110 项 / 103 条）。
>
> 目的：让一个**零上下文**的会话，只读一份文件就能接着干，且不重做已完成的事、不踩已踩过的坑。

## 1. 交接单列表（最新在最上）

| 文件 | 日期 | 版本串 | HEAD | 一句话 |
|---|---|---|---|---|
| [HANDOVER_v0121_shortcuts.md](docs/handover/HANDOVER_v0121_shortcuts.md) | 2026-11-25 | 0.12.1（**未发版**） | 工作区未提交 | **快捷方式闭环 + 开始菜单动态扫描 + Dock 固定图标修复**：① 开始菜单不再读写死的表 —— 启动期扫 `/apps`、`/usr/share/applications`、`/apps/desktop` 的 `*.lnk`/`*.desktop`（`name=/en=/exec=/icon=`）投影进注册表，按 exec 去重、扫入项**不写盘**、`fd64_close` 钩子认出快捷方式目录即重扫（实时同步）；root 幂等播种 4 份（图片查看器 `/bin/viewer`、MicroPython `/bin/mp`、编辑器、商店，**已存在不覆盖**）→ 图片查看器真的进开始菜单（`grid apps=14`）；② Dock「固定后变成黑底白字 A」根因：用户应用注册表 `id` 恒 `-1` + 开始菜单右键传了 `100+下标` → 改成 **exec 稳定哈希 id**（`/bin/viewer`=2694）+ 右键传注册表 id，Dock 图标恢复（新增 `[DOCK64] item … icon=` 判据）；③ 右键新增「打开文件所在位置」（按 exec 解析真实文件 → `explorer64_open_path64(path,0)` 跳目录并高亮）；验收 `shortcut64_test` **新增 22/22 PASS**、`version64` 8/8、`dock64` PASS；**两条 UI 点击判据未验证**（固定后图标、reveal）如实记在 §7 |
| [HANDOVER_v0120_smap_wp.md](docs/handover/HANDOVER_v0120_smap_wp.md) | 2026-11-25 | 0.12.0（**已发版**：tag `v0.12.0` = `905fc4cd…` / Release `457766679`） | `fde6a2c0` | **SMAP 真开 + WP 泄漏根因修正 + 三项复核**：① WP 那条"启动期泄漏"是**误判** —— 自检排在了初化之前（证据 `cr0_was=0x80000011` 正是 loader 打开分页时的值），顺序修正 + 判据改成"掉了就算 FAIL" + 进桌面前改成**只查不补**（`[ISO64] wp check at gui-entry wp=1`），三次启动 `wp_leak=0`；② CR4.SMAP 打开（CPUID 支持时）+ 全内核用户访存点 18 处显式 `STAC/CLAC`（引用计数、可嵌套）+ 进内核统一 `CLAC` + 内核态违规**只杀进程/降级不 PANIC**；用"探针模式 + objdump 归位 rip"把违规点从 5 → 3 → 4 → 3 → 1 → **0** 收敛（交付构建 `ISO64_SMAP_PROBE=0`）；`-cpu max` 全启动 **`[SMAP64] violation` 0 条**；③ 复核：KPTI 实现未动、仍完整（UEFI 无 KPTI 的设计性缺口仍在）；验收 `iso64_test` **48/48**、`version64` 8/8、`micropython64` PASS、`deskfolder64` 64/74（= 基线）；ISO sha256 `a4db6d34…` / IMG `e4b90713…`，旧版 v0.11.9 附件逐条未动 |
| [HANDOVER_v0119_kpti.md](docs/handover/HANDOVER_v0119_kpti.md) | 2026-11-25 | 0.11.9（**已发版**） | `d159d953` | **KPTI 双页表（v0.11.8 封存项，本轮开工并交付、已发版）**：每进程两张 PML4（内核表完整 / 用户表 **PML4[511]=0**），共用私有 PDPT；固定 VA **6 GiB**（`0x180000000`，PDPT[6]）的共享入口区 = 50 条向量蹦床 + int/syscall 入口出口 + GDT/TSS 副本 + 入口/返回栈 + IDT；`PML4[255]` 非 present 数据槽互存对照表（切换零维护）；验收 `iso64_test` **41/41**（硬证据 `audit … khigh=0 … kind=user`）、`micropython64_test` PASS、`proc64_test` 67/67、`version64_test` 8/8、`deskfolder64_test` 64/74（= 基线）；tag `v0.11.9` = `1a3e1070…` / Release **`457607672`**；§8 = **遗留安全项详单**（SMAP 未开 / 启动期 WP 泄漏根因未定位 / UEFI 无 KPTI，每项含影响·补法·判据） |
| [HANDOVER_v0118_ring3isolation.md](docs/handover/HANDOVER_v0118_ring3isolation.md) | 2026-11-24 | 0.11.8（**未发版**） | `cca2c043`（基线） | **ring3 真隔离第一层**：新 `kernel/iso64`（开 CR0.WP + CR4.SMEP（CPUID 有才开）+ 四级页表审计 khigh/ident/uwin/outside/us_violation + 6 位自检 + 活进程探针 + 终端 `iso` 命令）；**真发现**：启动期 CR0.WP 被某路径清掉（自检检测到 → reassert 补上 → 日志留痕）；验收 `iso64_test` **32/32**（含 `-cpu max` 证 SMEP 真开）、`micropython64_test` PASS；当时 KPTI 按用户指令叫停并封存，**v0.11.9 已重开并交付**；SMAP 未做 |
| [HANDOVER_v0117_ready.md](docs/handover/HANDOVER_v0117_ready.md) | 2026-11-24 | 0.11.7（**已发版**） | `26516540` | **本轮封存单**：tag `d740f4d0…` / Release `456771997` / ISO 93,458,432 B sha256 `14498c26…` / IMG 42,684,416 B sha256 `55cb4037…` / 旧版 v0.11.5 附件 id+size+digest+updated_at 逐条未动（RESULT: PASS）；附 3 条未验证、下一轮 = ring3 真隔离 |
| [HANDOVER_v0117_deskfolder.md](docs/handover/HANDOVER_v0117_deskfolder.md) | 2026-11-24 | 0.11.7（**未发版**） | `65626737`（基线） | **桌面 = 真实文件夹**：新 `kernel/deskdir64`（列 `<home>/Desktop` 目录为真源 / 拖到桌面 = `fs64_rename_to64` 真移动 / 删除 = 真进 `/Recycle`）+ `vfs64_mut_gen64()` 写类世代号（终端里 `touch` → 0.25 s 内桌面出图标，帧内零 I/O）+ 回收站合并成一套数据（虚拟项 + `/Recycle`）+ 图标按扩展名分 5 档 + 贪吃蛇方向表真 bug（`w` 变向左、一步咬自己）；验收 deskfolder64 **64/74**、dragdrop64 **51/51**、desktopops64 **72/72**、version64 **8/8**；两条 UNVERIFIED（桌面图标拖进回收站的手势、窗口圆角像素判据） |
| [HANDOVER_v0116_recycle_micropython.md](docs/handover/HANDOVER_v0116_recycle_micropython.md) | 2026-11-25 | 0.11.6（**未发版**） | 工作区未提交 | **回收站**（`/Recycle`：删除=同卷改名进站、恢复、永久删除有界递归、清空；explorer 导航窗格/工具栏/右键菜单按视图换语义）+ **MicroPython 跑脚本**（`/mpy/calc.py`、`snake.py`，端口 `argc>1 → pyexec_file`）+ **程序窗口顶部两角 90° 弧**（`gfx64_glass_top64` / `corner_cut_mask`，系统栏不动）。脚本 E2E **未通过**（参数没进内核命令行），如实记在 §2；桌面=真实文件夹、开始菜单时间块色差未做 |
| [HANDOVER_v0115_ready.md](docs/handover/HANDOVER_v0115_ready.md) | 2026-11-24 | 0.11.5（**已发版**） | `2044d59d` | **本轮封存单**：v0.11.5 发布状态（tag `265c8e0a…` / Release `456278688` / ISO+IMG sha256 / 旧版 v0.11.3 附件逐条未动）+ 23 条剩余边界（删除不进回收站、非空目录不能删、拖拽只同卷、最大化最近邻、Dock 上限 8、系统图标无 48px 档、全量清扫未跑完…）+ 8 条新坑（原始区硬预算、亚克力判据、前台窗口、多提交 API 回放、tag message 结尾换行） |
| [HANDOVER_v0115_explorer_ops.md](docs/handover/HANDOVER_v0115_explorer_ops.md) | 2026-11-23 | 0.11.5（**未发版**） | `65e0b0bd` | 功能单：**拖拽进文件夹 = 真移动**（fs64_rename_to64；撞名/自环/只读卷一律拒绝并打点 `[UI] explorer drag move`）+ `fileops64_test` 三条过期判据按 v0.11.1 起的亚克力菜单重写 + 补"右键新建文本文件"验收 |
| [HANDOVER_v0114_cjk_icon.md](docs/handover/HANDOVER_v0114_cjk_icon.md) | 2026-11-23 | 0.11.4（**未发版**） | `e3ed8078` | 功能单：**中文标题**（31 号内核光栅化覆盖率位图给 Ring 3 合成器，`ink` 打点）+ **真图标**（32 号应用身份 / 33 号图标 RGBA；窗口标题栏 + Dock 运行项）+ 资源管理器地址栏三按钮换图标包 chevron（kind 21/74）；两条先红后绿的如实定性（Dock 回弹窄竞态 / `system.img` 被测试写脏的 `reduce_motion=1`） |
| [HANDOVER_v0113_ready.md](docs/handover/HANDOVER_v0113_ready.md) | 2026-11-23 | 0.11.3（**已发版**） | `40208a45` | **本轮封存单**：v0.11.3 发布状态（tag `d2968c83…` / Release `455130600` / ISO+IMG sha256 / 旧版未动）+ 17 条剩余边界（ASCII 标题、Dock 上限 8、最近邻缩放…）+ 全量清扫挂起说明 |
| [HANDOVER_v0113_viewer_dock.md](docs/handover/HANDOVER_v0113_viewer_dock.md) | 2026-11-23 | 0.11.3（**已发版**） | `40208a45` | 功能单：查看器三毒瘤（悬停指针窟窿=两个写者 / 自绘标题栏退场换 28 号装饰三按钮 / 滚轮缩放）+ 指针唯一写者 + 最大化按比例缩放 + Dock × 窗口生命周期（30 号 ctl 4/5 + 外部窗口表 + 活动胶囊）+ WM 标题 TTF 字模 |
| [HANDOVER_v0112_ready.md](docs/handover/HANDOVER_v0112_ready.md) | 2026-11-23 | 0.11.2（**已发版**） | `c9b43717` | **本轮封存单**：v0.11.2 发布状态（统一装饰/三按钮真语义/拖拽持久化）+ 三条遗留边界 + 下一轮 = Dock × 窗口生命周期 + 布局弹性评估 + 全量清扫进度 |
| [HANDOVER_v0112_wmdeco.md](docs/handover/HANDOVER_v0112_wmdeco.md) | 2026-11-23 | 0.11.2（**已发版**） | `34974344` | Ring 3 合成器（/bin/wm）也接管装饰 + 三按钮真实语义（29/30 号）；跨层拖拽/explorer 遗留项复跑确认 |
| [HANDOVER_v0111_wlsurfwin.md](docs/handover/HANDOVER_v0111_wlsurfwin.md) | 2026-11-22 | 0.11.1（**已发版**） | `94406e93` | 第二步 wl_surface→Window（28 号 set_decor，装饰归合成器）落地 + 跨层拖拽；v0.11.1 已打 tag、已建 Release（ISO+IMG） |
| [HANDOVER_v0111_dragdrop.md](docs/handover/HANDOVER_v0111_dragdrop.md) | 2026-11-22 | 0.11.0（WIP，未 bump） | `113d37c6` | 跨层拖拽（explorer→桌面）落地并通过验收；PE 窗口统一装饰未做，未发版 |


> 新单只**追加**到本表顶部；旧单永不覆盖、永不删除（审计链）。
## 2. 开工三步（不许跳）

```powershell
cd C:\Users\fanqi\Desktop\VimtuOS\Vimtu64

# ① 读最新交接单
Get-ChildItem HANDOVER_*.md | Sort-Object LastWriteTime -Descending | Select-Object -First 1

# ② 核对现场（三条）
git log --oneline -3
git status --short
(Get-Item build64\kernel64_os.bin, build64\loader64.bin, vimtu64-64.iso -ErrorAction SilentlyContinue) |
  ForEach-Object { "{0,-22} {1,12:N0} B  {2:yyyy-MM-dd HH:mm}" -f $_.Name, $_.Length, $_.LastWriteTime }

# ③ 核对能力清单（秒级，不跑 QEMU）
py -3 tests/status_report.py | Select-String '汇总'
```

* 三条**一致** → 接着干交接单里"下一步"。
* **不一致** → 以现场为准，先补一份新交接单再开工（否则你在错误前提上干活）。
* 交接单里标"阻塞"的先确认是否解除；**环境问题**（QEMU 端口被抢、宿主内存不足、宿主分辨率装不下）不要当代码缺陷。

## 3. 收工三步

1. 写 `HANDOVER_v<版本数字>_<slug>.md`（放仓库根目录；版本数字 = `build64.sh` 的 `VIMTUOS_VERSION` 去点，
   如 `0.11.1` → `v0111`；未 bump 就在 slug 里带 `wip`）。
2. 把新单追加到 §1 表格**顶部**。
3. 零上下文自检：把新单当成写给"从没见过这个项目的人"——他能只靠这份单跑起验证命令并知道下一步吗？

## 4. 交接单模板（段不可缺）

```markdown
# HANDOVER —— <一句话说明这批做完了什么>
- 日期 / 版本串（build64.sh VIMTUOS_VERSION）/ 分支·HEAD·tag / 是否推送

## 1. 现场（下个会话必须核对）—— 表格：内核字节、安装内核、loader、iso、disk.img、构建结果、能力清单、测试规模
## 2. 本批做了什么（每条带证据：file:line 或串口原文或 N/N PASS；没证据写"未验证"）
## 3. 改动的文件（表格）
## 4. 验收（实跑 + 可复现命令）
## 5. 未完成 / 阻塞（标明是否环境问题 + 解除条件）
## 6. 下一步（按序 ≤5 条，每条自带判据）
## 7. 不要重做 / 不要踩的坑
## 8. 口径过期（文档 vs 代码，发版前必须回写）
## 9. 环境已知问题（本机特有）
```

## 5. 硬规矩（都是踩出来的）

1. **只增不改**：旧交接单永不覆盖、永不删除；要更正就在新单里写"更正 X 单的 Y"。
2. **不许写"已完成"而没有证据**；做不到就写 `未验证`。
3. **数字现场取**（`Get-Item` / `status_report` / 测试输出），不要从上一份单复制。
4. **区分产品问题与环境问题**，两个方向都不许糊。
5. **下一步必须写判据**，不许写"继续优化 UI"这种没法验收的话。
6. 交接单**不放产物二进制/截图**，只放路径与数字。
7. 本批**没发版**就必须写明"没打 tag、没建 Release"及原因（本仓库纪律：功能没做完不许发版）。

## 6. 与仓库其他纪律的关系

- 证据三类：**串口打点 / 像素判据 / 宿主侧独立解析** —— 交接单引用它们，别只写"测过了"。
- `docs/发布流程.md`：版本号唯一真源在 `build64.sh`；发版时 tag == VERSION == 镜像内嵌串。交接单的"版本串"字段就是给这一步用的。
- `tests/status_report.py` 的汇总行是能力计数的权威。
- 会话内进度用 TodoWrite 给用户看；**跨会话进度只认交接单**。
