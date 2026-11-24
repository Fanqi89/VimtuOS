# HANDOVER —— v0.11.3：图片查看器三毒瘤 + 指针唯一写者 + 最大化缩放 + Dock × 窗口生命周期 + WM 标题 TTF 字模

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.3"`（已 bump、已重建、串口核对见 §5）
- 分支/HEAD：`main`（本轮提交见 §6）
- 上一版：`v0.11.2`（Release `454904809`，tag 对象 `ee169fb3…`）

## 1. 现场（下个会话必须核对）

| 项 | 值 |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,418,512 B**（闸门 15,728,640 B） |
| 安装内核 `build64/kernel64.bin` | 1,899,232 B |
| `build64/loader64.bin` | 4,090 B |
| `/lib/wm.elf`（Ring 3 合成器） | **40,160 B**，PT_LOAD span **42,592 B**（装载窗口 64 KiB） |
| `/bin/viewer`（`build64/viewer.elf`） | 118,640 B |
| ISO / IMG / 磁盘镜像 | 93,437,952 / 42,663,936 / 38,469,120 B |
| 构建 | `bash build64.sh` → **EXIT=0**（必须 `C:\msys64\usr\bin\bash.exe`，脚本内先 `export PATH=/usr/bin:/bin`） |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = **157**（`*_test.py` 148）；`TESTS` = **106 条登记 / 106 唯一**（新增 `docklife64_test.py`） |

## 2. 本轮做成了什么（按用户给的三步）

### 2.1 第一步：图片查看器的三个毒瘤

**① 鼠标悬停变"透明窟窿" —— 根因是**指针有两个写者**，取证在像素上：**

- 现场：查看器窗口 `rect=576,352,128,96`，指针停在 `cur=638,398`（`[UI] perf`），
  该点像素 = 图片的纯蓝 `(0,0,255)`，**16x16 箭头一个像素都没有** —— 用户看到的"窟窿"就是被抹掉的指针。
- 机理：合成器每收到一个输入事件就把窗口矩形重画一遍（`user/wm/wm.c` ⑪ → `wm_paint_rect`），
  而软件光标是内核算好后**先画进后备缓冲**的一层像素 —— 谁后画谁留下。
- 修法（单一写者）：注册了 Ring 3 合成器时，**内核停止画软件光标**
  （`kernel/gui64.cpp` `draw_cursor()` 开头 `wl64_composer_pid64()` 判据，打点
  `[GUI64] cursor skip (ring3 composer owns pointer) pid=N`）；合成器自己画：
  `user/wm/wm.c` 的 `wm_ptr_paint()`（与内核同一份 16 行箭头字形，画在**每一帧最上层**），
  并把**旧位置 + 新位置**两个 16x16 包围盒都并进脏矩形（`wm_ptr_move`），否则旧箭头留屏。
- 实测：修后同一位置 `black=24 white=53` 像素（`verify_fixes.py`），`[GUI64] cursor skip` 出现。

**② 没有最大化/关闭按钮 —— 查看器改成走 28 号装饰（画家换成合成器）：**

- `user/viewer/viewer.c` 删掉自绘的 20px 标题栏 + 1px 边框（`CO_FRAME/CO_BORDER`），
  改为 `wl_surface_set_decor(surf, V64_WL_DECOR_WINDOW, "文件名 WxH")`；画布 = **客户区**（宽度 = 图宽，+0）。
- 于是标题带（25px）、三按钮（槽 30 / 块 24x16）、14px 圆角都由合成器按**唯一一份规格**画：
  有 `/lib/wm.elf` 就 wm 画（按钮可点），没有就走内核路。
- 关闭按钮：合成器发 `30 wl_surface_ctl(CLOSE)` → 内核先投 `type=7` CLOSE 事件再销毁 surface →
  查看器 `done frames=N reason=close` 退出（实测 `frames=1091 reason=close`）。
- 实测像素：标题带 `#F2F4F8` 1801/3200 px、旧自绘色 `0x3A4658` **0 px**、关闭按钮红底 367 px。

**③ 滚轮缩放失效 —— 内核一直有滚轮事件，查看器从来没读：**

- `kernel/input.cpp` 4 字节 Intellimouse 包的第 4 字节 → `EV64_TYPE_WHEEL`（`Ev64Event.dy` > 0 = 向上），
  `wl64_display_dispatch` 照原样投给客户端（`type=6`）。查看器只认 `KEY_DOWN/ESC`，所以"滚轮没反应"。
- 现在：`V64_EV_WHEEL` → `zoom_at(dir, 锚点)`：zoom 千分比（1000 = 适配画布，上限 4000），
  **以指针下的图像点为锚**（缩放时那个点不动），`paint()` 按 `zoom` 做最近邻取样（放大 = 裁剪视图）。
  打点 `[VIEWER] zoom now=1125 before=1000 at=640,400 off=0,0`；画布左上角画 `112%` 提示。
- 实测：3 次滚轮 → `zoom now=1125`，客户区像素真变了（diff=230 px，128x96 图放大 12.5% 的正常量）。

### 2.2 第二步：Dock × 窗口生命周期（任务栏真活了）

四个语义，全部有串口打点 + 像素判据（新 `tests/docklife64_test.py` **24/24**）：

| 语义 | 实现 | 判据 |
|---|---|---|
| 运行项登记 | 28 号声明成窗口 → `dock64_ext_add64(surf, title)`（`gui64.cpp` 外部窗口表，win_id = 10000+surf） | `[DOCK64] ext-add surf=1 win=10001 title=…` + `[DOCK64] item … kind=running win=10001 cx=.. cy=..` |
| 活动高亮 | 内核 `wl64_active_surface64()`（最高 id 且未最小化）；Dock 该条画**加宽胶囊**（70% 图标宽 × 4px，主题 accent） | `[DOCK64] extactive idx=9 surf=1 x=881 y=778 w=31 h=4 color=#0078D7`，胶囊像素 112/124 |
| 最小化收容 | 三按钮最小化 → `30 ctl action=4 minimize_to_dock`：`minimized=1`、**frame 高 0**（装饰+内容都不合成、不可命中）；Dock 收到 `ext-state` | `[WM] btn … action=minimize` + `[WL64] ctl … action=minimize … minimized=1` + `[DOCK64] minbar idx=9 …`；窗口矩形里红/蓝/标题带 **=0** |
| 点 Dock 恢复 | RUN 项点击 → `30 ctl action=5 restore`（`by=kernel`，见 §7 坑 1）；几何**不动**，所以必然逐像素回原位 | `[DOCK64] press … action=ext-restore` + `[WL64] ctl … action=restore … minimized=0`；**窗口矩形 diff = 0 / 12288 px** |
| 关窗退出运行态 | 关闭按钮 → `30 ctl CLOSE` → 销毁路径 `dock64_ext_del64` 收掉 Dock 项 | `[WL64] ctl … action=close notify=1 destroyed=1` + `[DOCK64] ext-del surf=1` + 客户端 `reason=close` |

新增内核 API：`int wl64_active_surface64()`；`kernel/dock64.h` 增 `dock64_ext_add64/del64/state64/title64`。
`dock64_close_window64()` 对 `win_id >= 10000` 改走 30 号 close（否则会留"Dock 项没了、窗口还在"的幽灵）。

### 2.3 第三步：最大化内容自适应缩放 + WM 标题换 TTF

**① 最大化缩放（两条合成路同一把尺子）**

- v0.11.2 最大化只把装饰框铺满，内容按原尺寸贴左上 → 右边/下边一大片客户区底色。
- 现在：`fit（保持宽高比）+ 居中`，`scale = min(cw*1024/sw, ch*1024/sh)`，多余两边仍是 `client_bg`（letterbox）。
  内核路 `kernel/wl64.cpp::wl64_blit_scaled64()`、wm 路 `user/wm/wm.c::wm_blit_scaled()` —— **逐式相同**
  （不缩放成非等比，否则圆形会变椭圆）。内核路打点 `[WL64] blitscale surf=1 from=128x96 to=<w>x<h> client=…`。

**② WM 标题换 TTF（不再 8x8 位图）**

- 想直接把 `user/lib/font64.c`（stb_truetype）链进 `/lib/wm.elf` **做不到**：ring3 的 ELF64 装载窗口是
  `4GiB .. 4GiB+64KiB`（`kernel/usermode64.h`），而 `font64.o` 一个文件 94,800 B；wm.elf 当时 36,032 B。
- 做法：`tools/wm_titlefont_win.py` 用**同一份字库、同一个 em(16px)** 预光栅化 ASCII 32..126 →
  `user/wm/wm_ttf.h`（4-bit alpha **4,718 B** + 95 条字形表）；wm 按字模做 alpha 混合（`wm_blend`），
  字形来源与内核窗口标题同源（face 2 = sarasa-mono-sc 子集），基线用"最高字形顶端 = 0"对齐内核的 `text_ttf(x,y)`。
- 结果：`/lib/wm.elf` **40,160 B**（PT_LOAD span 42,592 B，窗口 64 KiB 仍有余量）。
- 如实边界：字模只有 **Regular 一份 + ASCII**；非 ASCII 字符按 8px 空白跳过（标题里的中文会缺）；
  预光栅化没有 hinting，与内核运行时光栅化**同一套轮廓但不是同一份像素**。

## 3. 遗留边界（如实，不许抹）

1. **wm 标题只有 ASCII**：`wm_ttf.h` 是 ASCII 32..126 的预光栅化，中文标题在 wm 路下不显示（内核路正常）。
2. **最大化缩放是最近邻**：放大后是像素块（不做双线性），且**不改 surface 尺寸**（客户端自定尺寸不变）。
3. **Dock 只认"外部窗口表"**：`DOCK_EXT_MAX = 8`，超出的窗口不上任务栏（如实打 `ext-add` 表满）。
4. **最小化不改几何**：`restore` 靠"几何没动过"实现逐像素还原；如果将来允许最小化期间改位置，这条要重做。
5. **Dock 高亮的"活动"定义** = 最高 id 且未最小化；没有真正的焦点栈（内核 seat 的 focus 槽按 pid 存）。
6. `/bin/wm` 仍是有界进程（启动期 short 模式 20 s 收掉）；常驻要靠 `elfrun bg /lib/wm.elf` 或客户端自己 fork。
7. 指针唯一写者的代价：**wm 不画的地方就不会有指针**（dwm 式）。目前 wm 每帧都画，所以看不到缺口；
   将来若 wm 变成"只画脏区、其余不动"，必须给指针单开一个持久脏区。

## 4. 下一轮建议（按序）

1. **指针形状统一**：现在 wm 只会画箭头；内核还有文本 I 形 / 转圈 / 六向缩放 —— 合成器接管后这些形状丢了。
   做法：`22` 号 composer_get 顺手把当前形状带回来（或在 `23` 号 surface info 里加一字节），wm 按形状选字形。
2. **中文标题**：给 `wm_ttf.h` 加一份常用汉字子集（按卷里 `font_simhei.ttf` 的 charset 取 300~500 字），
   或按需把字模做成"卷里文件 + 首次用到时上传"。不要直接链 stb（装载窗口不够）。
3. **Dock 项图标**：外部窗口目前用默认图标（`app_id=APP_ID_NONE`）；应从客户端拿一个 app 身份
   （新 ABI 或 28 号 flags 里留位），才能复用 `/etc/iconpack.bin` 的真图标。
4. **布局弹性评估**（上一轮遗留）：`docs/布局弹性评估.md` 仍未产出。

## 5. 验收数字（本构建实跑）

| 脚本 | 结果 | 备注 |
|---|---|---|
| `tests/docklife64_test.py` | **24/24 PASS** | 本轮新增；A 运行项登记 / B 活动胶囊像素 / C 最小化收容（图片像素全 0）/ D **恢复后矩形 diff=0 / 12288 px** / E 关窗退出 |
| `tests/wmdeco64_test.py` | **24/24 PASS** | C 段按新语义重写（最小化 = 收容到 Dock → 点 Dock 项 restore） |
| `tests/viewer64_test.py` | **121/121 PASS** | 几何改 +0/+0；判据换合成器标题带 `#F2F4F8` + 关闭按钮红底（旧自绘色仍要求 0 px） |
| `tests/version64_test.py` | **8/8 PASS** | `build64.sh` == 内核内嵌 == 客户机串口（`VimtuOS 0.11.3 x86_64`） |
| `tests/wm64_test.py` | **89/89 PASS** | |
| `tests/wmfull64_test.py` | **PASS** | G 段：全程没有 PANIC / TRIPLE FAULT / selftest FAIL / composer 被 kill |
| `tests/deco64_test.py` | **14/14 PASS** | A0 含 `[WL64] selftest PASS`（move 27 + decor 28） |
| `tests/peload64_test.py` | **38/38 PASS**（1 env SKIP） | |
| `tests/dragdrop64_test.py` | **PASS** | 阶段 4/5「日志里不得出现 selftest FAIL」 |
| `tests/explorer64_test.py` | **90/90 PASS** | 文件管理器自检 + 全程无 selftest FAIL |

过程如实记录（两条都是**本轮自己引入**的缺陷，已修 + 复跑）：

1. **内核 30 号 ctl 的权限位**：`pid <= 0 -> EPERM` 让"点 Dock 项恢复"永远无效（第一版 docklife 的 D 段红：
   `[DOCK64] press … action=ext-restore` 有、`[WL64] ctl` 一行没有）。改成 `by_kernel` 放行后 D 段 diff=0。
2. **`wl64_selftest64` 的 30 号断言过期**：它仍要求内核上下文返回 `EPERM`，与第 1 条冲突 →
   首轮回归里 `wm64 / wmfull64 / deco64 / dragdrop64 / explorer64` 全部因"日志里出现 selftest FAIL"红。
   改成"没有 1 号 surface 时应为 `ENOENT`"，重建后 5 个脚本全部复跑绿（`regress_v0113_retry.log`）；
   `docklife64 / wmdeco64 / viewer64` 在新构建上**再跑一遍**确认（24/24、24/24、121/121）。

日志留档（scratch）：`regress_v0113.log`（首轮 10 个）、`regress_v0113_retry.log`（复跑 5 个）、
`verify_fixes.py` + `repro/A_park.ppm|B_hover.ppm`（"透明窟窿"前后取证的原始截图）。

## 6. 本轮改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/gui64.cpp` | `draw_cursor()` 在合成器注册时让出指针（`[GUI64] cursor skip`）；Dock 外部窗口表 + `ext-add/del/state/title`；`draw_dock` 外部窗口状态与**活动胶囊**；`dock_press64` RUN 分支走 30 号 ctl；`dock64_close_window64` 对外部窗口走 CLOSE |
| `kernel/wl64.cpp` | `minimized` 状态 + `frame64` 四口径；`wl64_hit64/top_of_pid64` 跳过最小化；`ctl` 新增 4/5（`by=kernel` 放行给 Dock 点击）；`wl64_active_surface64()`；`wl64_blit_scaled64()`（最大化缩放）；28 号登记 Dock、destroy 收 Dock；内核路 ctl 后 `gui64_dirty` 重画 |
| `kernel/wl64.h` | `WL64_CTL_MIN2DOCK64/RESTORE64`、`WL64_SURFINFO_MINIMIZED64`、`wl64_active_surface64()` |
| `kernel/dock64.h` | `DOCK_EXT_WIN_BASE` + 四个外部窗口 API |
| `user/wm/wm.c` | 指针 `wm_ptr_paint/wm_ptr_move`（单一写者）+ 每帧最上层；`wm_frame` 四口径；`wm_blit_scaled`；最小化按钮 → `MIN2DOCK`；TTF 标题 `wm_blend/wm_text_ttf`（替换 `wm_text8`） |
| `user/wm/wm_ttf.h` | **新增**（自动生成，4,718 B alpha + 95 字形，来源与生成命令写在文件头） |
| `user/wm/wmabi.h` | `V64_WM_MINIMIZED=0x40` |
| `user/lib/wl.h` | `V64_WL_CTL_MIN2DOCK=4`、`V64_WL_CTL_RESTORE=5` |
| `user/viewer/viewer.c` | 自绘标题栏退场（画家 = 合成器）；28 号装饰；滚轮缩放（锚点 + 千分比）；`type=7` CLOSE → `reason=close` |
| `tools/wm_titlefont_win.py` | **新增**：TTF → 4-bit alpha 字模（PIL） |
| `tests/docklife64_test.py` | **新增 24 条**（运行项登记/活动胶囊/最小化收容/点 Dock 恢复逐字节/关窗退出） |
| `tests/wmdeco64_test.py` | C 段按新语义重写：最小化 = 收容到 Dock（`minimized=1` + 标题带 0 px）→ 点 Dock 项 `restore` |
| `tests/viewer64_test.py` | 几何 +0/+0；判据 `0x3A4658` → 合成器标题带 `#F2F4F8` + 关闭按钮红底（旧色仍要求 0 px） |
| `tests/status_report.py` | 登记 `docklife64_test.py`（106 / 106） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.2 -> 0.11.3` |

## 7. 不要重做 / 不要踩的坑

1. **内核自己调 30 号 ctl 会被"pid<=0 → EPERM"挡掉**：Dock 点击发生在 ring0 的点击路径里，没有进程上下文。
   第一版没放行 → `[DOCK64] press … action=ext-restore` 打了，`[WL64] ctl` **一行都没有**，窗口永远不回来。
   现在 `by_kernel` 显式放行并在日志里写 `by=kernel`（用户态拿不到 pid<=0，越权面不变）。
2. **别只改一条合成路径**：装饰、缩放、指针三处都是"内核路 + wm 路"两份实现 —— 任何一处漏改就是
   "最大化只在内核路缩放、wm 路还是原尺寸"这种半截状态。
3. **指针别两边都画**：内核不停手就是两把箭头；wm 不画就是原来的"透明窟窿"。
4. **`[WL64] ctl` 的日志字段顺序不要打乱**（`action=… by=… collapsed=… maxed=… minimized=… frame=x,y,w,h`）：
   `wmdeco64_test` / `docklife64_test` 都按这条正则抓 `frame=0,0,<w>,<h>`。
5. **`#F2F4F8` vs `#F2F2F2` 仍需 tol=2**（v0.11.2 的坑，本轮沿用）。
6. **Dock 项坐标只能从 `[DOCK64] item … cx= cy=` 拿**（Dock 宽度随项数居中变化，别写死 896/754）。
7. **改 `wm.c` 的头文件包含**：`wm_font8.h` 已不再被 wm.c 使用（wmclock/wmpanel 自己 include），
   删包含后 `PL_FONT8_*` 就不可见了 —— 别把 `wm_text8` 留在文件里（`-Werror` 会因为未使用函数红）。
8. **别在回归跑着的时候重建**：每个测试都会 copy 一份 `build64/sysdisk.img`，重建到一半被 copy 就是坏盘。

## 8. 环境已知问题（本机特有）

1. `git push` 到 `github.com:443` 时通时不通；不通就走 Git Data API 回放（脚本见 scratch）。
2. 夹具 QEMU 会泄漏：跑完一批用 `Get-Process qemu-system-x86_64` 看一眼。
3. 宿主 8 GB：**两个 QEMU 并行会互挤死**（rc=4294967295，串口停在正常 `[TASK64] kheart`）。回归一律串行。
4. `bash` 必须解析到 msys2（`export PATH=/usr/bin:/bin`）。
5. PowerShell 下别用 `py -3 -c` 带中文引号/转义（会被 PS 吃掉），一律写 `.py` 脚本再执行。
