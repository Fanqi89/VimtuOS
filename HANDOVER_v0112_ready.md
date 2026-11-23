# HANDOVER —— v0.11.2 已发布（装饰统一 + 三按钮真语义 + 拖拽持久化）；本轮封存，下一轮 = Dock × 窗口生命周期

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.2"`（已 bump、已重建、已串口核对）
- 分支/HEAD：`main` @ `c9b43717`（docs 交接单；功能提交 `34974344`）
- tag：**`v0.11.2`**（annotated，tag 对象 `ee169fb373011eaa12c32af14543f31f684b60cc` → commit `34974344`）
- Release：**已发布** id `454904809`，`draft=false / prerelease=false`，附件 2 个
  - `vimtu64-64.iso` 93,433,856 B sha256 `07a5b48eafd1ef8e7428ca5a3e621486284f0f7645d6a0d8c0ca0df12af37cef`
  - `vimtu64-64.img` 42,659,840 B sha256 `59b25ee8db5e0253af359fd738af2085ec67b0ddfc8701be337b69ff926fe618`
  - **旧版本 v0.11.1 的两个附件 id/size/digest/updated_at 逐条未变**（发布脚本回读核对 PASS）
- 远端：`main` == 本地（逐位一致）；`git push` 到 github.com:443 当时不通，走 Git Data API 回放（见 §7）

## 1. 现场（下个会话必须核对）

| 项 | 值 |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,405,072 B**（闸门 15,728,640 B） |
| 安装内核 `build64/kernel64.bin` | 1,899,232 B |
| `build64/loader64.bin` | 4,090 B |
| ISO / IMG / 磁盘镜像 | 93,433,856 / 42,659,840 / 38,465,024 B |
| 构建 | `bash build64.sh` → **EXIT=0**（必须 `C:\msys64\usr\bin\bash.exe`，脚本内先 `export PATH=/usr/bin:/bin`） |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = **156**（`*_test.py` 147）；`TESTS` = **105 条登记 / 105 唯一** |

## 2. v0.11.2 发布状态（本版做成了什么）

1. **统一装饰（两条合成路都画，一份规格）**
   - 内核合成路：`gui64_deco_frame64 / gui64_deco_edge64`（v0.11.1 落地）。
   - Ring 3 合成器 `/bin/wm`：`wm_deco_frame / wm_deco_edge`（本版落地）——
     25px 标题带、三按钮（槽 30 / 块 24x16）、1px 边框、14px 圆角、8x8 位图标题。
   - 状态与规格一致：`WL64_DECOR_H64 = 25`、`THEME64_R_WINDOW = 14`、按钮右边距 1。
2. **三按钮真语义（不是像素）**：`30 wl_surface_ctl(surf, action)`
   - `close`：先给拥有者投 `type=7` CLOSE 事件，再销毁 surface；
   - `collapse`：折叠成 25px 标题带（客户区内容不合成），再点展开；
   - `maximize`：装饰框铺满整屏，内容按原尺寸贴左上（**不缩放**），再点还原。
   - 状态由**内核** surface 表持有；`23` 号导出 flags `0x8 decor / 0x10 collapsed / 0x20 maxed`；
     `29 wl_surface_title` 让合成器能读到标题去画标题栏文字。
   - 两条路都接了：wm 里点按钮走 30 号；内核合成路点标题带也走同一份 ctl，且**不投给客户端**。
3. **跨层拖拽持久化确认（上一批遗留项）**
   - `dragdrop64_test` **32/32 PASS**：拖拽建 `kind=9` 快捷方式 + `/etc/desktop.links` 落盘（`rc==bytes`）
     + **重启后** `[DESK64] links load why=init bytes=18 n=1 skipped=0 ok=1` 且 `[DESK64] item idx=3 kind=9 name=etc` 仍在。
   - `explorer64_test` **90/90 PASS**（资源管理器全链路）。

**验收数字（本构建实跑）**：`wmdeco64_test` 21/21、`wm64_test` 89/89、`wmfull64_test` 41/41、
`deco64_test` 14/14、`peload64_test` 38/38（1 env SKIP）、`dragdrop64_test` 32/32、
`explorer64_test` 90/90、`version64_test` 8/8（串口 `VimtuOS 0.11.2 x86_64`，旧串 0 次）。

## 3. 遗留边界（如实，不许抹）

1. **最大化不缩放内容**：surface 尺寸由客户端决定，内核只把装饰框铺满整屏、内容按原尺寸贴左上，
   多出来的区域铺客户区底色（`23` 号 `pad`）。想真"铺满内容"需要客户端配合或引入缩放合成。
2. **最小化只是折叠，不接任务栏**：`collapse` = 收成 25px 标题带（可逆、可验收），
   但**没有**"收进 Dock / 点 Dock 图标恢复 / 图标高亮当前活动窗口"这套语义 —— 这正是下一轮的核心。
3. **WM 标题字体与内核窗口不统一**：wm 用 8x8 位图（`user/wm/wm_font8.h`），内核窗口用 TTF；
   字高 8px 与位置对齐，但**字形不同**。统一需要把 `user/lib/font64`（stb_truetype）接进 wm。
4. `/bin/wm` 仍是**有界进程**：内核启动期 `wl64_wm64` 在无 `/etc/wm_probe` 时 20 s 收掉；
   验收用 `elfrun bg /lib/wm.elf` 起常驻。要让 wm 成为常态桌面唯一画家，需把 ring0 外壳外置（大工程）。
5. `wmfull64_test.py` 头注释里仍写旧标题栏色 `#3465A4` 的流程叙述（判据已换成 `#F2F4F8`，注释未逐句重写）。

## 4. 下一轮核心任务（按序，每条判据）

### 4.1 Dock 栏 × 窗口生命周期（**下一轮第一优先**）

目标：把 Dock 的"运行项/图标"与窗口生命周期真正打通。

1. **最小化收容**：`30` 号新增 `action=4 minimize_to_dock`（或复用 `collapse` 并加 dock 归属标记），
   窗口从可见集合摘掉、**内容不合成**，同时给 wm 一个"这条窗口在 Dock 第几项"的绑定。
   判据：点最小化后 `[WL64] ctl … action=minimize … dock=pending` + 屏幕上该窗口矩形内逐字节回到桌面背景；
   Dock 该项出现"运行中/最小化"标记（像素判据）。
2. **点击 Dock 恢复**：点 Dock 项 -> `30` 号 `restore`（清 collapsed/minimized、按 `pre_*` 还原几何）。
   判据：`[WL64] ctl … action=restore` + 窗口矩形与最小化前**逐像素**一致（screendump 比对）。
3. **高亮状态**：Dock 项必须反映"该 app 有窗口 / 当前活动窗口"。
   判据：活动窗口切换前后，Dock 对应项的**像素不同**（取色 + 计数），并新写 `docklife64_test.py`。
4. **关窗收尾**：`close` 之后 Dock 项要退出"运行中"（不能变成永远高亮的幽灵项）。
   判据：`[WL64] ctl … action=close` 后 1 s 内 Dock 该项像素回到"未运行"态。

### 4.2 窗口内控件的弹性布局（Layout）自适应评估

目标：先"评估"，再动手；不要一上来重写 UI 框架。

1. **测量**：挑 3 个真实窗口（设置页、资源管理器、PE 对话框），记录 客户区尺寸 -> 控件矩形 的映射
   现状（哪些是写死常量、哪些已按比例）。
   判据：产出 `docs/布局弹性评估.md`，逐个控件标 `fixed / ratio / min-max` 三类，并给出改造工作量估计。
2. **最小改造试点**：选设置页（已有 `settings64` 的卡片布局）先做 `min-max + 比例` 两档，
   判据：`settings64_test`（111 条）不回退 + 新增"客户区 640x480 与 1280x800 两种尺寸下无控件越界/重叠"断言。
3. **PE 对话框**：`MessageBoxA` 的 OK 按钮已按 `MB_W/MB_H` 计算，改造点只在"多行文本自动换行 + 最小尺寸"。
   判据：新语料 200x120 与 480x240 各一次，按钮矩形与文本行数都在界内。

## 5. 全量清扫进度（`status_report.py --full`，本轮回合封存时的真实状态）

- **启动**：2026-11-23 **09:58:29**（PID 9564 → 现 9164），命令 `py -3 tests/status_report.py --full`
- **进度（最后一次观测 11:53）**：日志 `$env:PI_SCRATCH_DIR\full_sweep_0112.log` = **756 行 / 124,333 B**，
  已推进到 `tests/gui_modern64_test.py`（PID 18796，11:51:51 启动）；此前已跑完 `multivol` 等一批
- **大面积崩溃判定**：**没有**。日志里没有出现任何 `FAIL` / `TIMEOUT` / `PANIC` 的**实跑**结论行
  （关键词命中全部是历史描述文字，如"绝不放宽断言""旧代码 timeout"）；`PANIC` 命中是"修前原文"引用
- **注意（本机环境问题）**：发现一个 10:11 启动的**孤儿 QEMU**（`multivol` 夹具）在测试子进程退出后仍挂着，
  已 `Stop-Process` 清掉（释放 ~512 MB，空闲内存回到 ~1.4 GB）。**夹具 QEMU 泄漏是已知环境问题**，不是产品 bug
- **收尾动作**：清扫**仍在跑**（105 个脚本 + 每个脚本内含多次开机，预计数小时）。下一个会话：
  先 `Get-Content $env:PI_SCRATCH_DIR\full_sweep_0112.log -Tail 40` 看汇总；若进程已不在且日志无汇总，
  就重跑 `py -3 tests/status_report.py --full`（一条链串行，别并行开第二个 QEMU）

## 6. 本轮改动的文件（提交 `34974344` + `c9b43717`）

| 文件 | 要点 |
|---|---|
| `kernel/wl64.h` / `kernel/wl64.cpp` | ABI 29 / 30；`wl64_frame64`（正常/折叠/最大化）；装饰改用 frame 矩形；blit 按 frame 偏移、折叠跳过内容；`wl64_hit_frame64`/`wl64_btn_hit64`；导出 flags + `pad=client_bg`；自检加 ctl 位 |
| `kernel/syscall64.cpp` / `.h` | 29/30 分派 + weak 声明 + 文档段 |
| `user/lib/wl.c` / `wl.h` | `wl_surface_title` / `wl_surface_ctl` 包装与常量 |
| `user/wm/wmabi.h` | 29/30 常量、`V64_WM_DECOR/COLLAPSED/MAXED`、原型 |
| `user/wm/wm.c` | 装饰模块（帧/按钮/圆角/位图字）+ `wm_compose_rect`（两条合成路径共用）+ 命中测试用帧 + 按钮 -> 30 号 + 标题读回 |
| `user/wm/wm_font8.h` | 新增（8x8 位图字体副本，注明来源与"与 TTF 字形不同"） |
| `user/win/peload_crt.c` | 收 `type=7` CLOSE 事件 -> `close by=composer` 分支 |
| `tests/wmdeco64_test.py` | 新增 21 条（装饰像素 + 三按钮全链路 + 客户端收 CLOSE） |
| `tests/wmfull64_test.py` | 3 处 `#3465A4` 判据换 `#F2F4F8`（强度不降，旧色仍要求 0；tol=2 防串色） |
| `tests/status_report.py` | 登记 `wmdeco64_test.py`（105 / 105） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.1 -> 0.11.2` |

## 7. 不要重做 / 不要踩的坑

1. **别只改一条合成路径**：`wm.c` 有"主合成 ⑨"和"输入后补画"两条，装饰必须都走 `wm_compose_rect`
   （本批实测：只改一条 -> 标题带全黑 (640,337)=(0,0,0)）。
2. **窗口状态别放合成器**：内核 surface 表是唯一源，否则"wm 里关了、内核又画回来"。
3. **`#F2F4F8` 与客户区面 `#F2F2F2` 在 tol=6 下串色**：像素判据用 tol=2（本批踩出 260 px 假红）。
4. **按钮坐标随框走**：最大化后按钮在 `(屏宽-91+i*30, 12)`，不能沿用原窗口坐标。
5. **调试常驻合成器用 `elfrun bg /lib/wm.elf`**（内核启动期那次只有 20 s）。
6. **点按钮的用例要放宽 peload 空闲超时**：`run /bin/peload <exe> 60000`（默认 8 s 不够连点五下）。
7. **发版门禁的串口证据只能用 ring3 程序**（终端 builtin 的 `cat/ver/uname` 不进串口）。
8. **PowerShell 下别用 `py -3 -` / heredoc**：改文件一律写 `.py` 脚本再执行。

## 8. 环境已知问题（本机特有）

1. **`git push` 到 `github.com:443` 时通时不通**：本批 `push_api_v0112.py` 用 Git Data API 逐位复现
   （blobs → tree → commit → PATCH ref），判据 = new tree sha == `HEAD^{tree}`、new commit sha == `HEAD`、
   递归树回读 14688 项 bad=0；tag 用 `git/tags` + `git/refs` 建（对象 sha 与本地一致）。脚本留档在
   `$env:PI_SCRATCH_DIR\push_api_v0112.py` / `rel_v0112.py`。
2. **夹具 QEMU 会泄漏**：`status_report --full` 期间出现孤儿 QEMU（父进程已退出），会吃掉 512 MB 并拖慢后续用例；
   收尾前 `Get-Process qemu-system-x86_64` 看一眼，确认没有孤儿再继续。
3. **宿主内存**：8 GB，两个 QEMU 并行就会互相挤死（rc=4294967295、串口停在正常 kheart、无 PANIC）。
   回归一律**一条链串行**跑。
4. **构建脚本 PATH**：`bash` 必须解析到 msys2（`export PATH=/usr/bin:/bin` 后再调），否则内层会走 WSL 的 bash。
