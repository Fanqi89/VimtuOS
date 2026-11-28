# HANDOVER v0.12.9 · ui-bugs —— 【最高优先级】焦点死锁 + 右键菜单渲染雪崩

- 日期：2026-11-27
- 版本串：**0.12.9（已发版）** —— tag `v0.12.9` = `987ca3d7…`（指向 `b4e31371`）/ Release **`460957623`**
  / ISO 97,529,856 B `c669f08e…` / IMG 46,754,816 B `e2d58dac…` / v0.12.4 旧附件逐条未动
- 关联单：`HANDOVER_v0129_cjk_tier3.md`（本单是它的**缺陷续作**）、`HANDOVER_v0128_ui_visual.md`
- 本单性质：**只记录用户实机报出的两个新缺陷 + 现场 + 排查方向**，零代码改动
- 现场：`kernel64_os.bin` = 3,610,144 B；工作区只剩 `Fonts/*.ttf`；QEMU = 0

---

## 0. 【最高优先级】两个缺陷（用户实机测试报出，先修这两个，别的都往后排）

### Bug 1 · 焦点死锁（多开之后键盘焦点切不过去）

**用户描述**：多开应用后，点击第二个窗口，键盘焦点切不过去，输入事件依然被旧窗口吃掉。

**本轮已拿到的同源证据**（v0.12.9 取证时撞到的，可直接复用）：

```
[WL64] seat get id=1 pid=26 flags=1 (focus-only: composer holds capture) users=1
[WL64] dispatch pid=26 composite=external pending=0 owner=27
```

实测复现路径（本轮做过，结论确定）：终端里先 `run /bin/viewer /kaisi.png` 起第一实例 →
**再在终端里敲任何命令，连 `[TERM] run argv` 都不会出现**（键盘事件根本没进内核窗口）。
当时为了做多开取证，只能改成**全鼠标**（资源管理器双击文件 = `exp_route_spawn64`）绕开它。

**排查方向（按优先级）**：

1. **`seat` / `capture` 的释放**：`kernel/wl64.cpp` 里 `seat get` 打点那句所在的分配逻辑
   （`flags=1` = focus-only）与 capture 的持有/释放；搜 `EV64_FLAG_CAPTURE` / `capture`
   （`kernel/ev64*` 或 `kernel/wl64.cpp` 的 `seat` 实现）。
   * 现象是"`composer holds capture` 之后再也不放" → 要确认**指针按下在别的窗口（尤其内核窗口）时**
     是否走 `capture-release`（历史上见过 `[EV64] focus pid=0 prev=25 reason=capture-release` 这条，
     说明释放通道存在，只是在这条路径上没被触发）。
2. **内核窗口被点击时是否重新申请焦点**：`kernel/gui64.cpp` 的 `handle_mouse_press()`
   → `gui64_set_active(w)`；内核窗口的键盘路由是否看 `w->active`，而 `active` 被 wl 侧
   `focus=` 覆盖/污染（搜 `wl64_focus` / `ev64_set_focus` / `gui64_set_active` 的交叉调用）。
3. **事件分发优先级**：`handle_keyboard()` 里取目标窗口的顺序（active → top → 无）。
   若 wl 合成器存在时把键盘整体交给合成器（`wl64_composer_pid64()` 分支），则需要
   "点到内核窗口就把 seat 收回来"的显式规则。
4. **判据（必须新增的自动化）**：
   - 起来一个 ring3 窗口 → 点内核终端 → 敲 `help` → 串口必须出现 `[TERM] cmd …` / `[TERM] run argv`；
   - 再点回 ring3 窗口 → 敲键 → `[EV64] … pid=<该 pid>` 必须跟着变；
   - 反复 3 轮，日志里**不得**出现只增长 `[WL64] seat get … holds capture` 而不出现释放的形态。

**风险面**：`tests/locklogin64_test.py`、`tests/wl64_test.py`、`tests/wm64_test.py`、
`tests/clicklaunch64_test.py` 都断言 `[WL64] seat`/`focus` 相关行；改语义时要保留原有字段顺序。

### Bug 2 · 右键菜单渲染雪崩（硬直角黑块 + 文字重叠 + 缺笔画）

**用户描述**：资源管理器右键菜单背景变成**硬直角黑块**，文字严重重叠、缺笔画；
怀疑**脏矩形未清理**或**遮罩错误**。

**已知的绘制现状**（v0.12.8 改过这里，先怀疑这轮的改动）：
`kernel/explorer64.cpp` 的 `draw_ctxmenu()` 现在：

```cpp
p2ui_popup64(x, y, EXP_CTX_W, h, THEME64_POP_R, THEME64_POP_R,
             t, t->client_bg, t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);   // 底板（圆角+亚克力+阴影）
const int line_h = p2ui_line_h64();
for (k …) {
    const int iy = y + 2 + k * EXP_CTX_ITEM_H;                                   // EXP_CTX_ITEM_H = 26
    if (en && hover) gfx64_fill_round64(x+4, iy+2, W-8, ITEM_H-4, R_BUTTON, t->sel_bg, 110);
    if (en) p2ui_fill_circle64(x + 10, iy + ITEM_H/2, 3, t->accent, 220);        // ★ v0.12.8 新增圆点
    int ty = iy + (ITEM_H - line_h) / 2;  if (ty < iy + 1) ty = iy + 1;          // ★ v0.12.8 改居中
    txt_clip(x + 22, ty, exp_ctx_label(k), fg, EXP_CTX_W - 30);                  // ★ 左移 12 → 22
}
```

**排查方向（按优先级）**：

1. **"硬直角黑块"= 圆角裁切的 backdrop 是黑的**：`p2ui_popup64`（`kernel/startmenu64.cpp:236`）
   用 `gfx64_wall_blur64()` 的整屏模糊面做角落 backdrop；`gfx64_corner_cut*` 在拿不到 blur 面时
   会填 `theme64_tokens64()->desktop_base`（深色）。本轮把 `gui64_deco_edge64()` 的
   `gfx64_corner_cut_mask64(mask)` 从 `0x3` 改成 **`0xF`**（四角全切）——**每帧多切两个角 =
   多两次 blur 采样**，如果 blur 面在某 region 未就绪/失效，就会表现为"直角 + 黑块"。
   * 查：`gfx64_wall_blur64(&bw,&bh)` 是否返回非空、`bw/bh` 是否覆盖菜单矩形；
     必要时把 `p2ui_popup64` 的圆角实现改成"不清 backdrop、只画描边+阴影"的退化路径并打点。
2. **文字重叠**：`line_h = p2ui_line_h64()` 用的是**当前字体面**（`font_select(2)` 后的度量）。
   若调用前 face 不是 2（`draw_ctxmenu` 之前 `draw_props/draw_addr` 改过），`line_h` 可能变成
   别的值 → `ty` 计算错 → 逐行偏移累积成"重叠"。**修法**：`p2ui_line_h64()` 前显式
   `font_select(2)`，或用 `EXP_CTX_ITEM_H` 与固定行高做二次钳制（`ty` 必须落在 `[iy+1, iy+ITEM_H-1-line_h]`）。
3. **缺笔画**：本轮给 UI 加了 **gamma 校正（`ui.gamma`=60）+ 近竖直边 1/4 px 吸附 + AA_SUB 16**。
   在**小字号 + 深色底 + 亚克力半透明**叠加上，覆盖率被抬到 76% 后可能与大色块混成一片
   → 看起来"糊/缺笔画"。**反证办法**：把 `/etc/fonts.conf` 的 `ui.gamma` 设 0 复现一次，
   若菜单文字恢复正常 → 就是 gamma 在深底场景下过头（考虑按背景亮度选 gamma，或只对浅底用 60）。
4. **脏矩形未清理**：`exp_draw()` 里菜单只把 `[x, y, EXP_CTX_W, exp_ctx_height()]` 标脏；
   `p2ui_popup64` 的**双层阴影外扩**（`sh_near/sh_far`）与 1px 高光边在矩形**外面**，
   菜单关闭/移动后会留残影。**修法**：把标脏矩形按 `THEME64` 的阴影外扩量（约 8~14 px）外扩，
   或菜单开关时整块内容区标脏一次。
5. **判据（必须新增的自动化）**：右键内容区 → 截图 → 断言
   ① 菜单四角都是圆角（用 v0.12.8 已建立的"最长亮带 + 顶/底行缩进"结构测量，**不要**用颜色阈值：
   这套浅色主题窗口底色与壁纸只差 1~6 级）；② 菜单区域内"硬直角"像素为 0（角点亮度远离主体均值）；
   ③ 菜单行数 × 行高 == 菜单高度（文字不重叠的几何判据）；④ 关闭菜单后原矩形区域与"从未开过菜单"
   的截图逐像素一致（**脏矩形清干净的硬判据**）。

**风险面**：`tests/fileops64_test.py` 阶段 4 就是这条菜单的权威判据（亚克力主体亮度 >= 210 /
灰度差 <= 12 / 1px 高光边 >= 周长一半 / 四角切开 / 菜单有文字）。改绘制时这 5 条必须继续过。

---

## 1. v0.12.9 已交付的东西（不要重做）

| 事项 | 结论 | 证据 |
|---|---|---|
| 中文渲染 | 轻量自动 hinting（近竖直边吸附 1/4 px）+ 覆盖率 gamma（`ui.gamma` 默认 60）+ `FONT64_AA_SUB` 8→16 | `tier3_shot` 中英混排：中文 ink=2168/dark_pct=27.8%/12 档，英文 12348/13 档 |
| 回收站拖拽 7 条 | 根因是**夹具**只按一次 `Ctrl+Shift+W`（内核只最小化最上面一个窗口）→ 循环最小化 + 修 3 处过期正则 + 1 处过期口径 | `deskfolder64_test` **76/76** |
| 多开 | ① Dock 对卷程序 `default: break;` → 按 app_id 反查注册表走 `app_launch_exec64`；② `appload` 单槽 → 深度 4 环 | 同一张图双击两次 → viewer pid=26/28、两个独立 `[WM] surf` |
| 幽灵调试窗 | `syscall64_screen_puts64()` 让每个 ring3 `write(1)` 都在 (8,8) 画白字黑底并绕过合成器 → 改夹具开关（`/etc/lxconsole.on`，默认关） | 起 viewer 前后左上暗像素 0/0（旧 145→1570） |
| 浏览器进菜单 | 种子 `/apps/browser.lnk` + `build64.sh` 补上**从未调用**的 `browser_pack_win.py` | 注册表 entries=15、`/bin/browser` 2,774,704 B 逐字节回读 |

**v0.12.9 回归基线（改动前必须以此为参照）**：
`version64` 8/8 · `deskfolder64` 76/76 · `fileops64` 117/117 · `startmenu64` 63/63 ·
`dock64` PASS · `https64` 22/22 · `rand64` PASS · `netping64` PASS · `iso64` 48/48 ·
`tier3_shot` 11/11。

---

## 2. 下一轮开工顺序（严格按这个顺序）

1. **Bug 1 焦点死锁**（先修这个：它让"多开之后继续操作"整条路断掉，也挡住 Bug 2 的自动复现，
   因为复现要靠键盘/鼠标交替）。
   - 先写"复现夹具"：起一个 viewer → 点终端 → 敲 `help` → 断言 `[TERM] …`；
   - 再按 §0 的四条方向逐条验；每条改动都要保留 `[WL64] seat`/`[EV64] focus` 的既有字段。
2. **Bug 2 右键菜单**（在焦点能自由切换之后再修，否则右键位置都不可信）。
   - 先加"菜单几何 + 圆角 + 脏矩形清干净"三条判据（§0-5），跑出**红**；
   - 按 §0 的 1→4 顺序逐个排除（blur backdrop → line_h → gamma → 脏矩形外扩）。
3. 收尾：`deskfolder64` / `fileops64` 两条必须回到 76/76 与 117/117，再发 v0.12.10。

---

## 3. 现场核对（四条命令）

```
git log --oneline -3          -> b4e31371 / c802fed5 / 1c377f48
git status --short            -> 只剩 Fonts/*.ttf（+ Fonts/fms_metadata.xml）
ls -l build64/kernel64_os.bin -> 3,610,144 B
Get-Process qemu-system-x86_64 -> 0
```

## 4. 本轮踩坑（别重复）

- **`Ctrl+Shift+W` 只最小化一个窗口**（`top_visible_win64()`）：任何"要在桌面/窗口上点到东西"的
  夹具都必须循环最小化，或先断言窗口真的没了。
- **ring3 合成器 hold 住 seat/capture 后内核窗口拿不回键盘**（= Bug 1 的现场）：
  取证时改**全鼠标**路径（资源管理器双击 = `exp_route_spawn64`）。
- **`screendump` 与双缓冲有时机差**：截图必须"带验证连拍"（先用内容见证点证明窗口真在/真不在）。
- **过期判据三连**：内核日志加字段（`entries=/total=`）→ 旧正则 `\S+` 永远不匹配；
  `why=` 成功时为空；口径依赖"回收站不生效"。修脚本时**判据本身不改**。
- **`sendkey` 的 `/` 是 `slash`**，否则静默丢失。
- **`fileops64_test` 的 `selftest FAIL` 可能是日志交织假红**（`[DESK64] selftest PASS mask=` 与
  `[TASK64] kwork` 抢行）→ 重跑一次再判定。
