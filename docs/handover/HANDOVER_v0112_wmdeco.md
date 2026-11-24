# HANDOVER —— v0.11.2 发版：Ring 3 合成器（/bin/wm）接管装饰 + 标题栏三按钮真实语义；跨层拖拽遗留项确认

- 日期：2026-11-23（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.2"`（**已 bump、已重建、已用串口核对**）
- 分支/HEAD：`main` @ `34974344`（v0.11.2: Ring 3 合成器接管窗口装饰 + 三按钮真实语义）
- tag：**`v0.11.2`**（annotated，tag 对象 sha `ee169fb373011eaa12c32af14543f31f684b60cc`，指向 `34974344`）
- Release：**已建** id `454904809`，`draft=false / prerelease=false`，附件 2 个（见 §4）
- 远端：`main` == 本地 `34974344`（**逐位一致**；git push 到 github.com:443 又不通，改走 Git Data API 回放，见 §9）

## 1. 现场（下个会话必须核对）

| 项 | 值（2026-11-23 实测） |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,405,072 B**（闸门 15,728,640 B） |
| 安装内核 `build64/kernel64.bin` | 1,899,232 B |
| `build64/loader64.bin` | 4,090 B |
| `vimtu64-64.iso` | 93,433,856 B，sha256 `07a5b48eafd1ef8e7428ca5a3e621486284f0f7645d6a0d8c0ca0df12af37cef` |
| `vimtu64-64.img` | 42,659,840 B，sha256 `59b25ee8db5e0253af359fd738af2085ec67b0ddfc8701be337b69ff926fe618` |
| 构建 | `C:\msys64\usr\bin\bash.exe <run_build.sh>`（脚本里先 `export PATH=/usr/bin:/bin`）→ **EXIT=0** |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = **156**（`*_test.py` 147）；`status_report.TESTS` = **105 条登记 / 105 唯一** |

## 2. 本批做了什么（每条带证据）

1. **`/bin/wm` 也画装饰**（`user/wm/wm.c`）
   - 新增装饰模块：标题带 `WM_DECO_HDR=25`、圆角 `WM_DECO_R=14`、三按钮槽 30 / 块 24x16、
     标题文字用 8x8 位图字体（`user/wm/wm_font8.h`，从 `user/win/peload_font8.h` 复制并说明来源）、
     四角圆角用 **wm 自己的整屏背景快照**补像素。
   - 证据：`[WM] title id=1 text="vimtu-peinfo"`；screendump 像素 标题带 2977/3000、旧色 0、close 红底 367、四角 201/784。
2. **三按钮真实语义：新 ABI `30 wl_surface_ctl(surf, action)`**
   - `close`：先给拥有者投 `type=7` 的 CLOSE 事件，再销毁 surface；
   - `collapse`：折叠成 25px 标题带（内容不合成），再点展开；
   - `maximize`：装饰框铺满整屏，内容按原尺寸贴左上（**不缩放**，多出的区域铺客户区底色），再点还原。
   - 证据：`[WL64] ctl id=1 pid=26 action=collapse by=composer collapsed=1 maxed=0 frame=490,325,300,25`
     → `collapsed=0` → `action=maximize … maxed=1 frame=0,0,1280,800` → `maxed=0`
     → `action=close … notify=1 destroyed=1` → `[PELOAD] msgboxA close by=composer surf=1` → 进程退出 0。
3. **状态只有一份，放内核 surface 表**：`23` 号导出 flags 新增 `0x8 decor / 0x10 collapsed / 0x20 maxed`，
   `pad` 改为客户区底色。内核路与 wm 路都读这一份 —— 否则"wm 里关了、内核又画回来"。
4. **`29 wl_surface_title(surf, out, cap)`**：合成器读标题（画标题栏文字用）。
5. **内核合成路也接管标题带**：按在三按钮上走同一份 ctl；按在标题带别处**不投给客户端**。
6. **本批抓到的真缺陷（自证留痕）**：第一版只把装饰接进 `wm_paint_rect`（输入后补画那条路），
   主合成路径 ⑨ 仍直调 `wm_blit_rect` → 标题带**全黑**（screendump (640,337)=(0,0,0)）。
   修法 = 抽出 `wm_compose_rect`（装饰前半 → 内容 → 装饰后半），两条路径都走它。
7. **三处过期判据按新语义换色（强度不降）**：`wmfull64_test.py` 里 `#3465A4` → `#F2F4F8`，
   并且仍然要求旧色 **0 px**（H① 新旧都查、H② 新位置查新色 + 旧色、E 段旧位置回填查新色 tol=2）。

## 3. 改动的文件

| 文件 | 要点 |
|---|---|
| `kernel/wl64.h` / `kernel/wl64.cpp` | 29/30 号、`wl64_frame64`（正常/折叠/最大化）、`wl64_deco_paint64` 用 frame 矩形、blit 按 frame 偏移并在折叠时跳过内容、`wl64_hit_frame64`/`wl64_btn_hit64`、导出 flags+pad、自检 +ctl 位 |
| `kernel/syscall64.cpp` / `.h` | 29/30 分派 + weak 声明 + 文档 |
| `user/lib/wl.c` / `wl.h` | `wl_surface_title` / `wl_surface_ctl` 包装与常量 |
| `user/wm/wmabi.h` | 29/30 号常量、`V64_WM_DECOR/COLLAPSED/MAXED`、两个原型 |
| `user/wm/wm.c` | 装饰模块（帧/按钮/圆角/位图字）、`wm_compose_rect`、命中测试用帧、按钮 -> 30 号、标题读回 |
| `user/wm/wm_font8.h` | **新增**（8x8 位图字体副本，注明来源与差异） |
| `user/win/peload_crt.c` | 收到 `type=7` CLOSE 事件走 `close by=composer` 分支 |
| `tests/wmdeco64_test.py` | **新增** 21 条断言（装饰像素 + 三按钮全链路） |
| `tests/wmfull64_test.py` | 3 处 `#3465A4` 判据换新语义（tol=2 防与客户区面串色） |
| `tests/status_report.py` | 登记 `wmdeco64_test.py`（105 / 105） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.1 -> 0.11.2` |

## 4. 验收（实跑，全部本构建）

| 脚本 | 结果 | 关键证据 |
|---|---|---|
| `tests/wmdeco64_test.py` | **21/21 PASS** | 见 §2.1/2.2 的串口与像素原文 |
| `tests/wm64_test.py` | **89/89 PASS** | Ring 3 合成器既有判据不回退 |
| `tests/wmfull64_test.py` | **41/41 PASS** | 拖动/真鼠标点 OK 全过；标题带 3381 px（新色）/ 0 px（旧色） |
| `tests/deco64_test.py` | **14/14 PASS** | 内核合成路装饰不回退 |
| `tests/peload64_test.py` | **38/38 PASS**（1 env SKIP） | 六份 PE 语料全过 |
| `tests/dragdrop64_test.py` | **32/32 PASS** | 跨层拖拽 + `/etc/desktop.links` 落盘 + 重启后 `links load n=1 skipped=0 ok=1`（上一批遗留项确认） |
| `tests/explorer64_test.py` | **90/90 PASS** | 资源管理器全链路（上一批遗留项确认） |
| `tests/version64_test.py` | **8/8 PASS** | 门禁：`0.11.2` == 内核内嵌串 == 客户机串口 `VimtuOS 0.11.2 x86_64`；旧串 0 |

复现命令：
```powershell
py -3 tests\wmdeco64_test.py      # ~3.5 分钟（新功能）
py -3 tests\wmfull64_test.py      # ~7 分钟
py -3 tests\wm64_test.py          # 快
py -3 tests\deco64_test.py        # ~2.5 分钟
py -3 tests\version64_test.py     # ~2 分钟（发版门禁）
```

## 5. 未完成 / 阻塞

- ★ **全量清扫（`py -3 tests/status_report.py --full`）已在后台启动但没跑完**：105 个脚本、预计数小时。
  日志 `$env:PI_SCRATCH_DIR\full_sweep_0112.log`。**在它跑完之前不许说"全绿"**。
  下次开工第一件事：看那份日志的汇总，FAIL/TIMEOUT 逐条定性。
- **`/bin/wm` 仍是有界进程**：内核启动期那次 `wl64_wm64` 没有 `/etc/wm_probe` 时 20 s 就收掉
  （串口 `[WL64] wm done … composer=0`）；验收用 `elfrun bg /lib/wm.elf` 起常驻。
  要让它成为常态桌面的唯一画家，需要把内核外壳（gui64/panels/dock）也外置 —— 大工程，另开批次。
- **最大化不缩放内容**（surface 尺寸由客户端定）；**最小化是折叠**（wm 还没有任务栏）。
- **wm 标题文字是 8x8 位图**，与内核窗口的 TTF 字形不同（字高/位置一致）。

## 6. 下一步（按序，每条带判据）

1. **收全量清扫**：`full_sweep_0112.log` 出汇总；判据 = 0 FAIL / 0 TIMEOUT（SKIP 只允许 env 类）。
   有 FAIL 就按 `stale-test-assertion-cross-version-triage` 先分"判据过期"还是"真回归"。
2. **三按钮的键盘通路**：`Alt+F4` = close、`Win+↓` = collapse、`Win+↑` = max（都走 30 号）。
   判据：`decobtn64_test.py` 用 `sendkey` 触发三条 ctl 打点，且 PE 对话框收到 CLOSE 后退出 0。
3. **wm 接管桌面底色**：让 wm 画壁纸/图标层，内核外壳只在没有 wm 时兜底；
   判据 = `wm64_test` 的整屏断言 + 新"wm 退出后桌面由内核接回"用例。
4. **标题字体统一**：给 wm 接 `user/lib/font64`（TTF），与内核窗口同一字形；
   判据 = wmdeco 的标题文字像素与内核路"同样字高、同样起点"，并在 RELEASE 里写清字形差异消失。

## 7. 不要重做 / 不要踩的坑

1. **别只改一条合成路径**：`wm.c` 里有"主合成 ⑨"和"输入后补画"两条，装饰必须走同一个
   `wm_compose_rect`（本批实测：只改一条 -> 标题带全黑）。
2. **别把窗口状态放在合成器里**：两条合成路共用一份（内核 surface 表 + 23 号 flags），否则关掉的窗会"复活"。
3. **`#F2F4F8`（标题带）与 `#F2F2F2`（客户区面）在 tol=6 下会串色**：像素判据要 tol=2，否则拖动后
   旧位置检查会把新窗口的正文底算成"标题色"（本批实测 260 px 假红）。
4. **按钮坐标随框走**：最大化后按钮在 `(0 + 屏宽 - 91 + i*30, 12)`，不能还用原窗口坐标（本批第一版就点空了）。
5. **`elfrun bg /lib/wm.elf` 才是常驻合成器**：内核启动期那次只有 20 s（short 模式）。
6. **`run /bin/peload <exe> <idle_ms>` 可以放宽对话框空闲超时**（夹到 1..120 s），点按钮的用例必须放宽，
   否则 8 s 内点五六下会超时。
7. **发版门禁的串口证据只能用 ring3 程序**（终端 builtin 的 cat/ver/uname 不进串口）。

## 8. 口径过期

- 本批没有新增过期项；v0.11.1 的 11 条已在上一批回写。
- 注意：`wmfull64_test.py` 头注释里"`[PELOAD] msgboxA close reason=clicked`"这段流程仍然成立，
  但标题栏颜色已改为合成器色（正文已更新，头注释未逐句改）。

## 9. 环境已知问题（本机特有）

1. **`git push` 到 `github.com:443` 不通**（`Recv failure: Connection was reset` / `Failed to connect after 21s`），
   `api.github.com` 200。本批做法：`scripts` 里 `push_api_v0112.py` 用 **Git Data API 逐位复现**本地提交
   （blobs → tree → commit → PATCH ref），判据 = new tree sha == `HEAD^{tree}`、new commit sha == `HEAD`、
   递归树回读 14687 项 bad=0。**因此远端 main 与本地 sha 逐位相同**（34974344），不是"等价 commit"。
   tag 用 `git/tags` + `git/refs` 建，tag 对象 sha 与本地一致（ee169fb3）。
2. **宿主内存**：另开 QEMU 会互相挤死；一条回归链串行跑（本批就是这样）。
3. **`py -3 - <<EOF` / `py -3 -` 在 PowerShell 下会挂**：改文件一律写 `.py` 脚本再执行。
4. **screendump 的 PPM 是 P6，1280x800**：像素判据统一用 `tests/deco64_test.py` 的 `read_ppm/px_at/count_region`。
