# HANDOVER —— v0.12.1（快捷方式闭环 + 开始菜单动态扫描 + Dock 固定图标修复）

- 日期：2026-11-25（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.12.1"`（唯一真源）
- 基线：v0.12.0（`fde6a2c0`，SMAP 真开 + WP 根因修正；**已发版**：tag `v0.12.0` / Release `457766679`，见 §6）
- 内核：**3,575,808 B**（闸门 15,728,640 B）；构建 `EXIT=0`
- 起因：用户文档 `"快捷方式解析闭环"和"开始菜单动态扫描".txt`（桌面 ⇄ 开始菜单 ⇄ Dock 入口统一；
  开始菜单必须**动态扫描**快捷方式目录；从开始菜单"固定到 Dock"后图标变成黑底白字 A）

## 0. 一句话结论

**开始菜单不再读写死的表，读的是"快捷方式目录"；图片查看器/MicroPython 真的进了菜单；
Dock 固定项丢图标的根因（用户应用 id 恒为 -1）修掉了。**

`tests/shortcut64_test.py`（**新增，22 条**）**22/22 PASS**，串口硬判据：

```
[APP64] shortcuts seed dir=/apps wrote=4 kept=0 ok=1
[APP64] shortcuts scan dirs=3 added=2 entries=14 fnv=00000000718B14E4 ok=1
[APPREG64] app idx=12 app=2694 name=图片查看器 en=Image Viewer exec=/bin/viewer icon=image-viewer builtin=0
[APPREG64] app idx=13 app=9129 name=MicroPython en=MicroPython exec=/bin/mp …
[START64] grid apps=14 cells=8  … | /bin/viewer | …
（第二次启动）[APP64] shortcuts seed … wrote=0 kept=4 ok=1   ← 幂等，不覆盖用户改过的
（第二次启动）[APP64] shortcuts scan … added=2 entries=14    ← 条目数与首次完全一致（不膨胀）
```

## 1. 开始菜单：从"写死的表"改成"快捷方式目录"

### 1.1 唯一真源 = 快捷方式文件

目录（按顺序扫，存在就扫）：**`/apps`、`/usr/share/applications`、`/apps/desktop`**
文件：`*.lnk` / `*.desktop`，纯文本 `key=value`（键名大小写不敏感；`[Desktop Entry]` 段同样适用）：

```
name=图片查看器
en=Image Viewer
exec=/bin/viewer
icon=image-viewer          # 系统图标名（icons64 的 kIcon64Names）或以 '/' 开头的卷内 PNG 路径
```

语义：
- **注册表（`/etc/apps.json`）是投影，不是真源**：启动期先 内置表 → 解析 `/etc/apps.json` →
  **扫描快捷方式目录**（按 `exec` 去重）→ 开始菜单/桌面/Dock 都从这里取；
- **扫进来的条目不入 `/etc/apps.json`**（`AppRegRec64::scanned = 1`，`ar_serialize64` 跳过）——
  否则"删掉 `/apps/x.lnk`"之后那条记录会永远赖在 json 里；
- 文件夹里**新增/删除**一个 `.lnk` → `fd64_close64` 钩子（写者上下文）→ `app64_registry_on_close64`
  认出"这是快捷方式目录"→ 重扫 + `gen++` → 开始菜单下一帧重建 → **实时同步**。

### 1.2 启动期播种（幂等，root 上下文）

`app64_boot_seed_shortcuts64()`（`kernel64.cpp` 里紧跟 `trash64_boot_ensure64`）：

| 文件 | name | exec | icon |
|---|---|---|---|
| `/apps/image-viewer.lnk` | 图片查看器 | `/bin/viewer` | `image-viewer` |
| `/apps/micropython.lnk` | MicroPython | `/bin/mp` | `terminal` |
| `/apps/text-editor.lnk` | 文本编辑器 | `/bin/edit` | `files` |
| `/apps/app-store.lnk` | 商店 | `/bin/store` | `plus` |

**已存在的不覆盖**（用户改过的不被启动期踩掉）。播种后扫描器把它们投影进注册表；
`/bin/edit`、`/bin/store` 因为已在内置表里被 `exec` 去重（所以 `added=2` 而不是 4 —— 这是对的）。

## 2. Dock「固定后变成黑底白字 A」的根因与修法

### 2.1 根因（两处，缺一不可）

1. **用户应用的注册表 `id` 恒为 `-1`**（`ar_parse64` 里 `a.id = -1;`），而 Dock 的固定列表
   （`/etc/dock.conf` 的 `pinned=<id>`）存的就是这个 id →
   `dock_reg_index64(app_id)` 用 `app64_reg_id64(i) == app_id` 查，**永远查不到** →
   标题落回 `"app"`、图标 `-1` → 界面画兜底字母 **A**（"此电脑"是内置 `APP_ID_MYPC`，所以正常）。
2. **开始菜单右键菜单传错了东西**：`dockmenu64_open_for_app64(sm_app_no64(view), …)` ——
   `sm_app_no64` 对用户应用返回 `100+下标`，**根本不是注册表 id**。

### 2.2 修法

- `ArRegRec64::id` 对用户应用 = **稳定哈希 id**：`ar_stable_id64(exec) = 1000 + FNV1a(exec) % 15000`
  （只依赖 `exec` 串 → 跨重启恒定；内置 `APP_ID_*` 都 < 1000，`APP_ID_NONE = -1`，不冲突）。
  实测：`/bin/viewer` → **2694**，`/bin/mp` → **9129**。
- 开始菜单右键改传 `app64_reg_id64(g_view[view].reg)`。
- 于是 `dock_app_icon_of64(app_id)` 能查到 `icon=image-viewer` → `icons64_kind_by_name64` = **74**（`ICON64_A_IMAGE`），
  Dock 画真图标，不再兜底。
- **新增判据打点**：`[DOCK64] item …` 行尾追加 ` icon=<icons64 kind>`（`icon=-1` 就是"画兜底字母"那张脸）。
  `-1` 现在只可能出现在真的没有图标的应用上。

### 2.3 存量 dock.conf

旧的 `pinned=-1`（如果用户此前固定过用户应用）无法自动改成新 id ——
**`/etc/dock.conf` 里那种 -1 项需要"取消固定再重新固定"**。新固定的一律正确。

## 3. 右键「打开文件所在位置」

`kernel/dockmenu64.cpp`：菜单新增 `DOCKMENU_ACT_REVEAL`（"打开文件所在位置" / "Open file location"），
**只在能解析出卷里真实文件时出现**（`app64_reg_exec_is_file64()`）。动作：

```
app_id -> 注册表下标 -> exec("/bin/viewer")
  -> explorer64_open_path64(exec, 0)     // 真实签名是 (path, letter)：它自己拆目录并**高亮文件名**
  -> [DOCK64] menu act=reveal exec=/bin/viewer dir=/bin
```

- 开始菜单与 Dock 共用这一个菜单 → 两边都有；
- ⚠ `kernel/explorer64.h` 里的 `explorer64_open_path64(const char*)` **过期**（真实现是两参数），
  按头文件调用会 `undefined symbol`（本轮踩过）。`dockmenu64.cpp` 里直接按真签名声明，不 include 那个头。

## 4. 「文件管理器双击真实应用能启动」

**既有能力，本轮没改**：`kernel/explorer64.cpp` 的双击路由已经分派
（VAP64 → `app64_launch64`；ELF → `elf64_run64`；扩展名关联 → 预览器），`tests/viewer64_test.py`
走的就是"双击 `/Pictures/x.png` → 路由 → `/bin/viewer`"这条真路径。
本轮**没有**回归它（见 §7 未验证项）。

## 5. 验收（实跑，串行）

| 脚本 | 结果 | 说明 |
|---|---|---|
| `tests/shortcut64_test.py`（**新增 22 条**） | **22/22 PASS** | 播种/扫描/注册表/开始菜单视图/二次启动幂等 全绿 |
| `tests/version64_test.py` | **PASS 8/8** | 版本串 0.12.1；旧串 v0.12.0 在内核里 0 次 |
| `tests/dock64_test.py` | **RESULT: PASS** | 固定/取消固定/重启后从 `src=file` 恢复（ids=7,9,1,2,5,4,6）/运行项合并 全绿 —— id 语义改动没有破坏 Dock |
| 构建 | `EXIT=0` | `kernel64_os.bin` **3,575,808 B** |

## 6. v0.12.0 发版回读（本轮先发的那一版）

| 项 | 值 |
|---|---|
| tag | `v0.12.0` = `905fc4cd22ed519a06d730d2b14625e882a90eaf` → commit `fde6a2c0` |
| Release | id **`457766679`**（draft=false / prerelease=false，附件 2 个 `uploaded`） |
| `vimtu64-64.iso` | 93,458,432 B — `sha256:a4db6d342cc0b5360a709330fb0a08d170956b4fa6e5a8e7cced4503a7c2f799` |
| `vimtu64-64.img` | 42,684,416 B — `sha256:e4b907130ebafb5187efb7448e3a30cf6e15f482a8d75b5b53407da9005d9871` |
| 旧版 v0.11.9 | 两附件 `id/size/digest/updated_at` **逐条未动**（RESULT: PASS） |

## 7. 本批**没做 / 未验证**的（如实点名）

> **两条 UNVERIFIED 的统一原因：当前自动化点击环境受限。** 本仓库的鼠标注入只有 QEMU monitor 的
> **相对移动**（`mouse_move dx dy` + `mouse_button`），要点中一个具体控件必须靠"C 光标打点闭环"把
> 光标先同步到已知位置再逐步逼近（`tests/dock64_test.py` 的 `sync_cur` / `goto_exact`）；本轮没有把
> 这套闭环接到"开始菜单 tile / 上下文菜单行"上（tile 坐标还要按 theme Token 现算），所以这两条
> **只用串口数据 + 代码路径证明，没有端到端点击证据**。判据留着，下一轮补自动化即可。

1. **"从开始菜单固定到 Dock 后图标正确" = UNVERIFIED（自动化点击受限）**：数据与代码路径已对上
   （`app=2694` + `icon=image-viewer` + `dock_reg_index64` 命中 + 新增 `[DOCK64] item … icon=` 打点），
   但**没有**一次"真点击固定 → 读 `icon=74`"的端到端证据。
   手工验证一条命令即可：开始菜单里右键"图片查看器"→ 固定到 Dock → 串口搜
   `[DOCK64] item … name=图片查看器 … kind=pinned icon=74`。
2. **右键"打开文件所在位置" = UNVERIFIED（自动化点击受限）**：菜单行、动作、打点
   `[DOCK64] menu act=reveal exec=/bin/viewer dir=/bin` 都已就位，缺一次真实点击。
   手工验证：开始菜单或 Dock 上右键"图片查看器"→ 点"打开文件所在位置"→ 串口出现上面那行 +
   文件管理器停在 `/bin` 且 `viewer` 高亮。
3. `tests/viewer64_test.py`（双击启动真实应用）本轮**没跑**（既有能力，见 §4）。
4. 桌面图标右键**没有**加"打开文件所在位置"（本批只做了开始菜单 + Dock 共用的那个菜单）。
5. `/usr/share/applications`、`/apps/desktop` 两个目录**只扫不建**（存在才扫）；播种只建 `/apps`。
6. 存量 `dock.conf` 里的 `pinned=-1` 不自动迁移（见 §2.3）。
7. 发版状态见 §11（**v0.12.1 已发版**）。

## 8. 改动文件

| 文件 | 要点 |
|---|---|
| `kernel/app64.h` | `APPREG64_MAX` 24→**48**、`APPREG64_FILE_MAX` 8192→**16384**（48 条才装得下，否则 `ar_serialize64` 返回 0 → 写盘失败）；新增 `app64_shortcuts_scan64 / app64_boot_seed_shortcuts64 / app64_reg_exec_is_file64` |
| `kernel/app64.cpp` | `AppRegRec64::scanned`（扫入项不写盘）；`ar_stable_id64`（exec → 稳定 id）；`ar_scan_shortcuts64`（扫 3 个目录、解析 .lnk/.desktop、exec 去重）；`ar_kv64/ar_ends_ci64`；`ar_reload64`/`ar_reload_and_bump64` 接扫描；`app64_registry_on_close64` 认快捷方式目录（实时同步）；`g_sc_dirty` 让"删掉 .lnk"也能重建 |
| `kernel/startmenu64.cpp` | 右键菜单传**注册表 id**（原来传 `100+下标`） |
| `kernel/dockmenu64.cpp` | 新增 `DOCKMENU_ACT_REVEAL`（"打开文件所在位置"）+ 行数上限 4→5 + `dm_reg_of_app64/dm_dir_of_exec64` + 按**真签名** `explorer64_open_path64(path, letter)` 调用 |
| `kernel/gui64.cpp` | `[DOCK64] item …` 行尾追加 ` icon=<icons64 kind>`（Dock 图标判据） |
| `kernel/kernel64.cpp` | 启动期 `app64_boot_seed_shortcuts64()`（root 幂等播种） |
| `build64.sh` | 0.12.0 → **0.12.1** |
| `tests/shortcut64_test.py` | **新增** 22 条（播种/扫描/注册表/开始菜单视图/二次启动幂等/无 PANIC） |

## 9. 下轮建议

1. 把 §7.1/§7.2 的两条 UNVERIFIED 补成鼠标自动化（`tests/dock64_test.py` 里已有
   `sync_cur` / `goto_exact` / `mon.raw(["mouse_button 2", …])` 这套基建可直接抄）；
2. 桌面图标右键也加"打开文件所在位置"（`desktopops64` 的菜单）；
3. 播种 `/usr/share/applications`（标准路径）并让 vpkg 装包时**自己写 .lnk**（那时开始菜单就真的是"装完即出现"）；
4. 跑 `viewer64_test` / `startmenu64_test` / `deskfolder64_test` 完整回归，再发 v0.12.1。

## 10. 现场

- `VIMTUOS_VERSION="0.12.1"`；`kernel64_os.bin` **3,575,808 B**；构建 `EXIT=0`
- 打点总表：`[APP64] shortcuts seed|scan`、`[APPREG64] app …`、`[START64] grid apps=…`、
  `[DOCK64] item … icon=`、`[DOCK64] menu act=reveal exec=… dir=…`
- 复现：`py -3 tests/shortcut64_test.py`

## 11. 发版回读（v0.12.1 已发布）

| 项 | 值 |
|---|---|
| tag | <!--TAG--> |
| Release | <!--REL--> |
| `vimtu64-64.iso` | <!--ISOB--> |
| `vimtu64-64.img` | <!--IMGB--> |
| 旧版 v0.12.0 | <!--PREV--> |

发版脚本：`$PI_SCRATCH_DIR\rel_v0121.py`（与 v0.11.9 / v0.12.0 同一套：本地 annotated tag 与远端
tag 对象**逐位复现 sha**、Release + 两附件上传、本地 `sha256` 与远端 `digest` 双向核对、
上一版附件 `id/size/digest/updated_at` 逐条回读未动）。
