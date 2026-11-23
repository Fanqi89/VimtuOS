# HANDOVER —— v0.11.3 **已发布**（查看器三毒瘤 + 指针唯一写者 + Dock × 窗口生命周期 + 最大化缩放 + WM 标题 TTF 字模）

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.3"`（bump → 重建 → 串口核对全过）
- 分支/HEAD：`main` —— **功能基线 = `40208a45`（tag `v0.11.3` 指向它，Release 也挂在它上面）**；
  其后只有 docs 提交（封存单 + 两处口径修正），不改任何产物。当前 HEAD 以 `git log --oneline -1` 为准。
- tag：**`v0.11.3`**（annotated，tag 对象 `d2968c839c0da8f940d0c584aaf47695de9dfdcd` → commit `40208a45`）
- Release：**已发布** id **`455130600`**，`draft=false / prerelease=false`，附件 2 个
  - `vimtu64-64.iso` 93,437,952 B sha256 `169014f73652f281c34feb67918f8cd202281b06b4b259d22c3931edbdb20b54`
  - `vimtu64-64.img` 42,663,936 B sha256 `cd92e6cd1a288dd7a1adfb8de0887da485ec757bd4b16fc22d09c7bd7f7ef345`
  - **旧版 v0.11.2 的两个附件 id/size/digest/updated_at 逐条未变**（发布脚本回读核对 PASS：
    img id `781158208` size 42,659,840 updated `2026-11-23T01:58:24Z`；iso id `781157670` size 93,433,856 updated `2026-11-23T01:58:18Z`）
- 远端：`main` == 本地（`40208a45`）；tag 远端对象 sha 与本地**逐位相同**（`d2968c83…`）

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
| 测试规模 | `tests/*.py` = **157**（`*_test.py` 148）；`TESTS` = **106 条登记 / 106 唯一** |

## 2. 本版做成了什么（一句话版）

1. **图片查看器三毒瘤全除**：① 悬停"透明窟窿"根因是指针有**两个写者**（合成器重画窗口抹掉内核软件光标）→
   改成单一写者（内核让出 + wm 画在每帧最上层）；② 自绘标题栏退场，改走 28 号装饰 →
   最大化/关闭按钮由合成器画且语义真实（close 走 30 号 ctl，客户端收 `type=7` 后 `reason=close`）；
   ③ 滚轮缩放接上 `EV64_TYPE_WHEEL`（锚点 + 千分比）。
2. **Dock × 窗口生命周期**：`30` 号新增 `4 minimize_to_dock` / `5 restore`；内核外部窗口表
   （`dock64_ext_*`，win_id = 10000+surf）把 ring3 窗口接进任务栏：运行项登记 / 最小化横杠 /
   活动窗口加宽胶囊 / 关窗退出运行态。点 Dock 恢复后窗口矩形**逐字节相同（diff=0 / 12288 px）**。
3. **最大化内容自适应缩放**：`fit + 居中`，内核 `wl64_blit_scaled64()` 与 wm `wm_blit_scaled()` 逐式相同。
4. **WM 标题换 TTF 字模**：`tools/wm_titlefont_win.py` 用同一份字库、同一 em(16px) 预光栅化 ASCII 32..126 →
   `user/wm/wm_ttf.h`（4-bit alpha 4,718 B）。**为什么预光栅化**：`/lib/wm.elf` 的 ELF64 装载窗口只有 64 KiB，
   `font64.o` 目标文件 94,800 B —— 直接链 stb 塞不进去。

## 3. 剩余边界（如实，**一条都不许抹**）

### 3.1 字体 / 标题
1. **wm 标题只有 ASCII**：`user/wm/wm_ttf.h` 是 ASCII 32..126 的预光栅化，**中文标题在 wm 路下不显示**
   （非 ASCII 字符按 8px 空白跳过）；内核合成路的标题正常（那是运行期 TTF）。
2. **预光栅化 ≠ 运行期光栅化**：同一套轮廓、同一个 em，但**像素不同**（PIL 无 hinting）。
3. 字模只有 **Regular 单字重**、只有 ASCII；要中文得另加子集（下一轮建议见 §4）。

### 3.2 缩放 / 最大化
4. **最大化缩放是最近邻**：放大后是像素块（不做双线性）；`scale = min(cw*1024/sw, ch*1024/sh)`。
5. **最大化不改 surface 尺寸**：客户端自定尺寸不变，多余两边是 letterbox 的 `client_bg`。
6. 查看器自己的滚轮缩放也是最近邻 + **裁剪视图**（画布固定，放大看局部）；zoom 上限 4000‰、下限 = 适配画布。

### 3.3 Dock / 窗口生命周期
7. **Dock 外部窗口表上限 8**（`DOCK_EXT_MAX = 8`）：超出的窗口不上任务栏，串口如实打 `ext-add` 表满。
8. **"活动窗口"= 最高 id 且未最小化**：没有真正的焦点栈（内核 seat 的 focus 槽按 pid 存）。
9. **最小化不改几何**：`restore` 靠"几何没动过"实现逐像素还原；若将来允许最小化期间改位置，这条要重做。
10. **Dock 项图标**：外部窗口用默认图标（`app_id = APP_ID_NONE`），没有从客户端拿 app 身份。
11. **wm 的最小化按钮语义变了**：`v0.11.2` 是 `collapse`（折叠成 25px 标题带），`v0.11.3` 起是
    `minimize_to_dock`（整窗摘掉）。`collapse`（`ctl action=2`）仍在内核里可用，但**没有 UI 入口**了。

### 3.4 指针
12. **wm 只会画箭头**：内核还有文本 I 形 / 转圈 / 六向缩放 —— 合成器接管指针后这些形状丢失（形状状态没传下来）。
13. **指针唯一写者的代价**：wm 不画的地方就没有指针。当前每帧都画，所以看不到缺口；若将来 wm 改成
    "只画脏区、其余不动"，必须给指针单开一个持久脏区。
14. 内核在合成器注册期间**完全不画软件光标**（`[GUI64] cursor skip …`）；合成器崩了要等下一次鼠标移动才恢复。

### 3.5 进程 / 其它
15. `/bin/wm` 仍是**有界进程**（启动期 short 模式 20 s 收掉）；常驻靠 `elfrun bg /lib/wm.elf` 或客户端自己 fork。
16. **`30` 号 ctl 对内核上下文（pid<=0）放行**（Dock 点击发生在 ring0 路径）。用户态拿不到 `pid <= 0`，越权面不变，
    但这确实是权限模型的一次放松 —— 改动时要想清楚。
17. **布局弹性评估仍未做**：`docs/布局弹性评估.md` 不存在（上一轮遗留，本轮没碰）。

## 4. 下一轮建议（按序）

1. **指针形状统一**：`22 wl_composer_get` 顺手把当前形状带回（或在 `23` 号 info 里加一字节），wm 按形状选字形。
   现在合成器接管后只会画箭头 —— 这是本版最明显的"修好一个丢一个"。
2. **中文标题**：给 `wm_ttf.h` 加常用汉字子集（按卷里 `font_simhei.ttf` 的 charset 取 300~500 字），
   或做成"卷里文件 + 首次用到时上传"。**不要**直接链 stb_truetype（64 KiB 装载窗口不够）。
3. **Dock 项真图标**：给外部窗口一个 app 身份（新 ABI 或 28 号 flags 留位），复用 `/etc/iconpack.bin`。
4. **布局弹性评估**：先产出 `docs/布局弹性评估.md`（3 个真实窗口逐控件标 fixed/ratio/min-max + 工作量），
   再做设置页 min-max 试点（`settings64_test` 111 条不回退）。
5. **Dock 外部窗口表扩容**：`DOCK_EXT_MAX` 8 → 与 `WL64_MAX_SURFACES64` 对齐（或改成链表）。

## 5. 全量清扫（本轮重新挂起，**正在后台跑**）

- **启动**：2026-11-23 **16:00:38**（`py` PID 按实际，用 `Get-Process py` 查），命令
  `cmd /c "py -3 tests\status_report.py --full > <scratch>\full_sweep_0113.log 2>&1"`
- **为什么用 `cmd /c` 而不是 PowerShell 管道**：管道 `| Out-File` 会**攒到进程结束才落盘**
  （上一轮 09:58 启动的清扫，日志 2 小时只写了开头 124 KB 的能力清单就是这个原因）；`cmd /c` 的重定向是流式的。
  **但别把"日志没动"当成卡死**：① Python 自己按 8 KB 块缓冲标准输出；② `status_report.py --full` 是
  "每个脚本跑完再汇总打印"，所以日志是**阶梯式增长**（本轮 16:00 启动后 3 分钟仍是 124,575 B 的清单，属正常）。
  真进度看 `Get-Process py` / `qemu-system-x86_64` 的 PID 有没有换、StartTime 有没有更新。
- **日志**：`$env:PI_SCRATCH_DIR\full_sweep_0113.log`
- **收尾动作（下个会话）**：先 `Get-Content ...\full_sweep_0113.log -Tail 40`；
  进程不在且没有汇总 → 重跑（**一条链串行，别并行开第二个 QEMU**，宿主 8 GB 会互挤死）。
  FAIL 先按 `stale-test-assertion-cross-version-triage` 分"判据过期"还是"真回归"。
- 上一轮（v0.11.2 时代）的清扫在 11:5x 被主动停掉以腾机器做本轮开发，日志留档
  `full_sweep_0112.interrupted.log`（无任何实跑 FAIL 结论行）。

## 6. 本版改动的文件（提交 `40208a45`）

| 文件 | 要点 |
|---|---|
| `kernel/gui64.cpp` | `draw_cursor()` 合成器注册时让出指针；Dock 外部窗口表 + `ext-add/del/state/title`；`draw_dock` 外部窗口状态 + 活动胶囊；`dock_press64` RUN 分支走 30 号 ctl；`dock64_close_window64` 对外部窗口走 CLOSE |
| `kernel/wl64.cpp` | `minimized` 状态 + `frame64` 四口径；`hit64/top_of_pid64` 跳过最小化；`ctl` 4/5（`by=kernel` 放行）；`wl64_active_surface64()`；`wl64_blit_scaled64()`；28 号登记 Dock / destroy 收 Dock；内核路 ctl 后 `gui64_dirty`；selftest 30 号断言改 `ENOENT` |
| `kernel/wl64.h` | `WL64_CTL_MIN2DOCK64/RESTORE64`、`WL64_SURFINFO_MINIMIZED64`、`wl64_active_surface64()` |
| `kernel/dock64.h` | `DOCK_EXT_WIN_BASE` + 四个外部窗口 API |
| `user/wm/wm.c` | 指针 `wm_ptr_paint/wm_ptr_move`；`wm_frame` 四口径；`wm_blit_scaled`；最小化按钮 → `MIN2DOCK`；TTF 标题 `wm_blend/wm_text_ttf`（替换 `wm_text8`） |
| `user/wm/wm_ttf.h` | **新增**（自动生成，4,718 B alpha + 95 字形） |
| `user/wm/wmabi.h` | `V64_WM_MINIMIZED=0x40` |
| `user/lib/wl.h` | `V64_WL_CTL_MIN2DOCK=4`、`V64_WL_CTL_RESTORE=5` |
| `user/viewer/viewer.c` | 自绘标题栏退场；28 号装饰；滚轮缩放（锚点 + 千分比）；`type=7` CLOSE → `reason=close` |
| `tools/wm_titlefont_win.py` | **新增**：TTF → 4-bit alpha 字模（PIL） |
| `tests/docklife64_test.py` | **新增 24 条** |
| `tests/wmdeco64_test.py` | C 段按新语义重写（24 条） |
| `tests/viewer64_test.py` | 几何 +0/+0；判据换合成器标题栏（121 条） |
| `tests/status_report.py` | 登记 `docklife64_test.py`（106/106） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.2 → 0.11.3` |

## 7. 验收数字（本构建实跑，串行一条链）

| 脚本 | 结果 |
|---|---|
| `tests/docklife64_test.py` | **24/24 PASS** |
| `tests/wmdeco64_test.py` | **24/24 PASS** |
| `tests/viewer64_test.py` | **121/121 PASS** |
| `tests/version64_test.py` | **8/8 PASS**（`VimtuOS 0.11.3 x86_64`，旧串 0 次） |
| `tests/wm64_test.py` | **89/89 PASS** |
| `tests/wmfull64_test.py` | **PASS**（G 段无 PANIC/TRIPLE FAULT/selftest FAIL） |
| `tests/deco64_test.py` | **14/14 PASS** |
| `tests/peload64_test.py` | **38/38 PASS**（1 env SKIP） |
| `tests/dragdrop64_test.py` | **PASS** |
| `tests/explorer64_test.py` | **90/90 PASS** |

本轮**自己引入**并已修掉的两个缺陷（写在这里防复现）：

1. 内核调 30 号 ctl 被 `pid <= 0 -> EPERM` 挡掉 → "点 Dock 项恢复"永远无效（`[DOCK64] press` 有、`[WL64] ctl` 没有）。
   改 `by_kernel` 放行（日志 `by=kernel`）后 docklife 的 D 段 diff=0。
2. `wl64_selftest64` 的 30 号断言仍要求内核上下文返回 `EPERM`，与 1 冲突 → 首轮回归 5 个脚本因
   "日志里出现 selftest FAIL"红。改成"没有 1 号 surface 时应为 `ENOENT`"，重建后全部复跑绿。

## 8. 不要重做 / 不要踩的坑

1. **别只改一条合成路径**：装饰、缩放、指针三处都是"内核路 + wm 路"两份实现。
2. **指针别两边都画**：内核不停手就是两把箭头；wm 不画就是原来的"透明窟窿"。
3. **`[WL64] ctl` 日志字段顺序别打乱**（`action=… by=… collapsed=… maxed=… minimized=… frame=x,y,w,h`）：
   `wmdeco64_test`/`docklife64_test` 都按这条抓 `frame=0,0,<w>,<h>`。
4. **`#F2F4F8` vs `#F2F2F2` 需要 tol=2**（tol=6 会串色）。
5. **Dock 项坐标只能从 `[DOCK64] item … cx= cy=` 拿**（Dock 宽度随项数居中变化）。
6. **`wm_font8.h` 已不再被 wm.c 使用**（wmclock/wmpanel 自己 include）；删包含后 `wm_text8` 必须一起删
   （`-Werror` 会因未使用函数红）。
7. **别在回归跑着的时候重建**：每个测试都 copy 一份 `build64/sysdisk.img`，重建到一半被 copy 就是坏盘。
8. **长跑清扫用 `cmd /c` 重定向**，别用 PowerShell 管道 `| Out-File`（会攒到结束才落盘）。

## 9. 环境已知问题（本机特有）

1. `git push` 到 `github.com:443` 时通时不通：功能提交那次通了（`063eaf1c..40208a45`、`* [new tag] v0.11.3`）；
   **封存单那次又挂了**（`fatal: unable to access …: Empty reply from server`），最终用
   `$env:PI_SCRATCH_DIR\push_api_v0113.py` 走 Git Data API 逐位复现推上去
   （判据：new tree sha == `HEAD^{tree}` = `5c58700b…`、new commit sha == `95a292fb…`、递归树回读 14694 项 bad=0）。
   本地 `refs/remotes/origin/main` 已用 API 核实过的 sha 对齐（`git fetch` 不通时这么做，别留着过期 tracking）。
2. 夹具 QEMU 会泄漏：一批跑完用 `Get-Process qemu-system-x86_64` 看一眼再继续。
3. 宿主 8 GB：两个 QEMU 并行会互挤死（rc=4294967295，串口停在正常 `[TASK64] kheart`）。回归一律串行。
4. `bash` 必须解析到 msys2（`export PATH=/usr/bin:/bin`）。
5. PowerShell 下别用 `py -3 -c` 带中文引号/转义（会被 PS 吃掉），一律写 `.py` 脚本再执行。

## 10. 发布脚本留档（scratch）

- `$env:PI_SCRATCH_DIR\rel_v0113.py` —— 快照旧版附件 → 建/推 tag → 建 Release + 上传 → 双向核对（本轮 RESULT=PASS）
- `body_v0113.md` / `commit_msg.txt` —— Release 正文 / 提交信息
- `v0112_assets.json` —— 旧版附件快照（id/size/digest/updated_at）
- `regress_v0113.log`（首轮 10 个）、`regress_v0113_retry.log`（复跑 5 个）、`full_sweep_0113.log`（本轮全量清扫，正在跑）
- `verify_fixes.py` + `repro/A_park.ppm|B_hover.ppm` —— "透明窟窿"修复前后的原始取证截图
