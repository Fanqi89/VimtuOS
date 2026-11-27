# HANDOVER v0.12.5 · ui-fix —— 第一梯队（桌面新建 / 资源管理器新建 / 拖拽幽灵）

- 日期：2026-11-27
- 版本链：`0.12.6/0.12.7` 阶段（**未 bump** `build64.sh` 的 `VIMTUOS_VERSION`；本单只到"改完代码/部分已编"）
- 关联单：`HANDOVER_v0126_browser.md`（浏览器）、`HANDOVER_v0125_netsurf_libs.md`（同名前缀，**不同主题**：那份是 NetSurf 依赖库）
- 本单起点：用户人工全量测试报出 11 个缺陷，按梯队修；**本单只做第一梯队**（文件系统状态一致性 + 拖拽）

> 用户原话（复现条件，极重要）：
> 「不是仅在根目录，我是在随便进入一个普通文件夹（或者就在桌面文件夹）里，点击右键菜单或者工具栏的新建，都没有反应。」
> 「跟 D 盘无关，我没有先访问只读盘，一开机就去 C 盘随便一个目录新建，也是失败的。」

---

## 1. 一句话

桌面「新建文件夹」的**真凶已修**（原来只塞内存项、从不落盘）；资源管理器新建的**判断层盲区已修**
（`g_ro` 缓存的是"当前卷"而不是"目标盘"）；拖拽幽灵的根因**已定位到具体行**（不是 surface 裁剪，
而是鼠标命中测试在出窗口后把事件交给了别人）——但 capture 旁路与偏移对齐**还没动手**。

---

## 2. 已完成（有提交）

| 提交 | 内容 |
|---|---|
| `02ea1752` | **桌面「新建文件夹」真落盘**：`kernel/desktopops64.cpp` 的 `DOPS_MENU_ID_NEWFOLDER` |
| `8e58caf8` | **资源管理器新建的只读位盲区**：`kernel/explorer64.cpp` 新增 `exp_recompute_ro64()` |

### 2.1 桌面新建（缺陷 #2 的真凶）

旧代码（改前）：

```c
case DOPS_MENU_ID_NEWFOLDER:
    ... DeskItem nit{}; nit.kind = slot; item_default_pos(...); g_items[g_n++] = nit;
    dbg64_str(" persisted=1 (desktop shortcut entry; no /Desktop volume in this batch)");
```

也就是说**只往内存 `g_items[]` 塞了一条虚拟项**，日志自己都承认"本批没有 /Desktop 卷"。
而 v0.11.7 起桌面图标已经改成一**列 `~/Desktop` 真目录**（`kernel/deskdir64.cpp`）。
两套模型并存 → 桌面"新建"的文件夹在 `~/Desktop` 里根本不存在（缺陷 #2），
反过来终端/资源管理器往 `~/Desktop` 写文件桌面能显示。

现实现：

```c
const char* base = gui64_tr("New folder", "新建文件夹");
const char* dir  = deskdir64_dir64();
const int   slot = deskdir64_slot64();
deskdir64_unique_name64(base, uniq, sizeof(uniq));         // 撞名自动 (2)(3)…
s_cpy(full, dir); … full = dir + "/" + uniq;
rc = (slot >= 0) ? vfs64_mkdir_on64(slot, full) : vfs64_mkdir64(full);
if (rc == 0) desk_sync64("newfolder");                     // 目录已变 -> 立刻按目录重建图标
dbg64_str("[DESK64] newfolder name=… path=… rc=… persisted=1 real=1 files=N");
```

- 内核已重建通过：`build64/kernel64_os.bin` = **3,597,296 B**。
- 回归：`tests/deskfolder64_test.py` = **64/74 PASS**，与 v0.11.7 基线**逐条相同**（无新增失败）。
- ⚠️ **仍未加运行时断言**：那套用例里没有"右键桌面 → 新建文件夹"的鼠标阶段，
  所以 `[DESK64] newfolder … rc=0 real=1` 目前只有代码路径、**没有串口实测**。→ 见 §4 第 3 步。

### 2.2 资源管理器新建（缺陷 #1）—— 已拿到关键证据 + 已改判断层

**实测证据**（`tests/fileops64_test.py`，本轮实跑）：

```
checks=117 ok=116            (唯一失败是视觉项"编辑框白底（黑=0）")
[PASS] mkdir /docs
[PASS] 新建文件夹成功（mkdir path=/newdir rc=0）
[PASS] 复制/剪切/粘贴/重命名/删除 全 PASS
```

→ **VFS 路径与权限在盘根是好的**（`rc=0` 真落盘）。所以锅在**判断层**，不是 `fs64_mkdir64`。

改法（`8e58caf8`）：`g_ro` 原本只在 `exp_refresh_dir()` 里由
`fs64_is_readonly64(-1)`（**当前卷**）算一次；只要"当前卷"和"界面上这个目录所属的盘"曾经不一致，
`g_ro` 就是上一次的结论 → 新建/粘贴/重命名被整体拦死（对应用户"一开机去 C: 随便哪个目录都建不了"）。

```c
static bool exp_recompute_ro64(void) {
    if (g_mode != 1) return g_ro;
    const int di = g_letter ? drive64_by_letter64(g_letter) : -1;
    if (di < 0) return g_ro;
    DriveInfo64 d;
    if (drive64_info64(di, &d) != 0) return g_ro;
    g_ro = d.readonly ? true : false;      // ★ 按"界面上这个盘"的只读位，与 fs64 的当前卷解耦
    return g_ro;
}
// exp_edit_begin():  if (g_ro) → if (exp_recompute_ro64())
```

⚠️ **未编译、未跑测试**（本单收尾时会编，但不宣称已验证）。

---

## 3. 拖拽幽灵：根因已定位到行（**尚未修**）

### 3.1 先把"不是它"的排掉

- **不是 surface 裁剪**：`kernel/gui64.cpp:3063` 在**合成阶段**直接调 `explorer64_drag_paint64()`
  （就在 `draw_cursor()` 前一行），所以幽灵本来就画在**屏幕顶层**。
- 矩形本身是对的：`exp_drag_ghost_rect()` 用屏幕坐标（`g_drag_x + 14, g_drag_y + 12`），
  还做了贴边钳制（`if (gx + EXP_DRAG_GW > sw) …`）。尺寸 `EXP_DRAG_GW=124 / EXP_DRAG_GH=66`。

### 3.2 真因（两条，互相独立）

1. **没有鼠标捕获**：`g_drag_x/g_drag_y` 只在 explorer 收到鼠标移动时更新；而 `gui64` 的鼠标分发是按
   **命中测试**（`gui64_hit_window64` / 窗口矩形）路由的 —— 指针一出窗口，事件就给了别人，
   `g_drag_x/y` 定格在窗口边缘，看起来就是"图标只在窗口内动、出窗就没了"。
   同时 explorer 收不到 `mouse-up` → `g_drag_active` 停住 → **`deskdir64_move_in64` 永远不会被调用**
   （这就是"真移动没触发"的直接原因）。
   `grep -n "capture|grab|mouse_owner|drag_owner" kernel/gui64.*` → **一个都没有**，机制确实缺失。
2. **两个绘制者、两套基准**：用户看到的"小白框 + 3cm 外的真图标"里，
   小白框是 `draw_cursor()` 的光标形状，真图标是 `explorer64_drag_paint64()`；
   前者按光标热点、后者按 `+14/+12` 偏移 —— 基准不同源，所以错位。

### 3.3 修法（下一轮照这个做，不要另起炉灶）

- **capture 旁路**：`gui64` 的鼠标分发处，当 `explorer64_drag_active64()` 为真时**跳过命中测试**，
  无条件把 motion / up 投给 explorer；并打点 `[GUI] drag capture on/off`。
  不需要新 syscall —— `explorer64_drag_active64()` 已经是现成的"我在拖"信号（导出于 `kernel/explorer64.h:103`）。
- **偏移同源**：幽灵偏移改成与光标热点同一份常量（消掉 3cm 错位）。
- 落点判定已有现成件，别重写：`exp_desktop_target()`（桌面空白）与 `exp_drop_dir64()`（拖进子目录），
  真移动走 `deskdir64_move_in64()` / `fs64_rename_to64()`。

---

## 4. 下一轮开工顺序（**严格按这个顺序，1→4**）

1. **`gui64` 鼠标分发加"拖拽期旁路命中测试"**
   - 位置：`kernel/gui64.cpp` 的鼠标事件分发（按 `gui64_hit_window64` 路由那段）
   - 判据：串口 `[GUI] drag capture on` / `off`；拖出窗口后 `[DESKDIR] move in why=explorer-drag … rc=0 ok=1` 出现
2. **幽灵偏移与光标热点同源**（消"3cm 错位"）
   - 位置：`kernel/explorer64.cpp` 的 `exp_drag_ghost_rect()`（`+14/+12`）
   - 判据：截图里幽灵与光标相对位置固定，且不随窗口位置变化
3. **补桌面新建的鼠标用例**
   - 位置：`tests/deskfolder64_test.py` 新增一个阶段（右键桌面空白 → 点第 k 项"新建文件夹" → 不打名字直接回车/或打名回车）
   - 判据：`[DESK64] newfolder … rc=0 persisted=1 real=1` + `[DESKDIR] scan … added=1` + 宿主侧读卷确认目录存在
4. **`exp_recompute_ro64()` 连同上面三项一起编译并跑测试**
   - `bash build64.sh`（~13 分钟）→ `py -3 tests/fileops64_test.py`（117 项）→ `py -3 tests/deskfolder64_test.py`（74 项）
   - 判据：fileops 的 `新建文件夹成功（mkdir … rc=0）` 仍 PASS；deskfolder 不低于 64/74 基线；新增阶段 PASS

---

## 5. 本轮踩坑 / 纪律（别重复）

- **`Edit` 工具行号错位本轮又踩 3 次**（改了 `vimtu_font.h` 的相邻行、把 heredoc 的 `#endif` 删掉）。
  → 一律用 **`old_string`/`new_string` 唯一字面串**替换，或写 `.py` 按唯一串替换并 `assert count==1`。
- **`DOPS_LINK_PATH_MAX` 不存在**：桌面那段路径缓冲要用 `DESKOPS_LINK_PATH_MAX`（第一次改就踩，编译期才发现）。
- **`build64.sh` 会清掉 `build64/netsurf/lib/*.a`**：改完内核若要再链浏览器，先
  `bash tools/netsurf_lib_build_win.sh` + `tools/netsurf_core_build_win.sh` + `tools/browser_build_win.sh`。
- 长跑一律串行（8 GB 宿主）；收尾必须 `Get-Process qemu-system-x86_64` = 0、`git status --short` 只剩 `Fonts/*.ttf`。

---

## 6. 现场核对（三条命令）

```
git log --oneline -3          -> 8e58caf8 / 02ea1752 / ed6e81d9
git status --short            -> 只剩 Fonts/*.ttf（第三方字体，历来不提交）
ls -l build64/kernel64_os.bin -> 3,597,296 B
Get-Process qemu-system-x86_64 -> 0
```

---

## 7. 用户报的 11 个缺陷与本单进度

| # | 缺陷 | 状态 |
|---|---|---|
| 1 | 资源管理器任意目录无法新建文件夹 | **判断层已改（`8e58caf8`），待编译+测试** |
| 2 | 桌面新建目录不进 ~/Desktop | **已改（`02ea1752`），内核已编，待补断言** |
| 3 | 拖拽移动失效 + 幽灵跟丢 | **根因已定位（§3），未修** |
| 4 | 终端字体太细、笔画丢失（h 像 n） | 未动（第二梯队） |
| 5 | 右键菜单样式崩坏 | 未动（第二梯队） |
| 6 | 窗口**底角**没圆角（只有上两角） | 未动；注意 `deskfolder64_test` 里已有三条圆角像素 FAIL 就是它 |
| 7 | Dock / 开始菜单不够精细 | 未动 |
| 8 | 开始菜单丢浏览器 | 未动（第三梯队） |
| 9 | 除终端外不能多开 | 未动 |
| 10 | 图片查看器双击无反应 | 未动（第四梯队） |
| 11 | 启动时左上角"调试小窗" | 未动（第四梯队） |
| 12 | 重命名输入框没有左右方向键光标 | 未动（第四梯队） |
