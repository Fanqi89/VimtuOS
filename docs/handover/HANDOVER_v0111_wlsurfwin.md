# HANDOVER —— v0.11.1 发版：第二步 `wl_surface → Window`（装饰归合成器）+ 跨层拖拽；已发布到 GitHub

- 日期：2026-11-22 23:05（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.1"`（**已 bump、已重建、已核对内嵌串**）
- 分支/HEAD：`main` @ `94406e93`（v0.11.1 发版：bump 0.11.1 + 发版门禁脚本 version64_test.py）
  - 前两笔：`3bd0377e`（第二步·装饰归合成器）、`113d37c6`（第一步·跨层拖拽）
- tag：**`v0.11.1`**（annotated，`rev-parse` = `019dc847c7d5dd5e5a8e872e9bb4008c3eb5078d`）
- Release：**已建** id `454519096`，`draft=false / prerelease=false`，附件 2 个（见 §4）
- 远端：`main` 已由 88f3dcba 快进到 `94406e93`（**注意：git push 到 github.com:443 当时不通，改用 GitHub API `PATCH /git/refs/heads/main`；详见 §9**）

## 1. 现场（下个会话必须核对）

| 项 | 值（2026-11-22 23:05 实测） |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,401,760 B**（闸门 15,728,640 B = 30,720 扇区） |
| 安装内核 `build64/kernel64.bin` | 1,899,184 B（**不含** `0.11.x` 字面量，属正常：settings/terminal 只编进系统内核） |
| `build64/loader64.bin` | 4,090 B |
| `vimtu64-64.iso` | 93,425,664 B，sha256 `66001cb4731b92ac83d61fcd48afccd4afc77a915c00ed71a6267dedd0a3e9cf` |
| `vimtu64-64.img`（安装介质） | 42,651,136 B，sha256 `b72ce07cf9d4c7b73148d20db0d3fcdc8265d42ac778086a032648b8f78a8777` |
| `vimtu64-64-disk.img` / `build64/sysdisk.img` | 38,456,320 B / 38,456,320 B |
| 构建 | `C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash build64.sh"` → **EXIT=0** |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = **155**（含新 2 个）；`status_report.TESTS` = **104 条登记 / 104 唯一** |

## 2. 本批做了什么（每条带证据）

1. **新 ABI `28 wl_surface_set_decor(surf, flags, title)`**
   - 语义/权限/错误码/打点定义点：`kernel/wl64.h` 的 `WL64_ABI_SURFACE_DECOR64` 段。
   - 反例：没有进程上下文一律 `WL64_EPERM64`（已进自检 bit2）。
   - 证据：`[WL64] selftest PASS (abi 15..21 + composer 22..26 + move 27 + decor 28 …)`
2. **装饰只有一份实现**（`kernel/gui64.cpp`）
   - 新导出 `gui64_deco_frame64` / `gui64_deco_edge64` / `gui64_deco_h64`；
     内核窗口的 `draw_window()` 与 `kernel/wl64.cpp` 的 `wl64_deco_paint64` 调**同一对函数**。
   - 自检里加了一条"两边同一把尺子"：`gui64_deco_h64() == WL64_DECOR_H64 == 25`，否则 `fail |= 16`。
3. **装饰分两相**（本批实测抓到的真缺陷）
   - 现象：第一版把整份装饰画在内容之后 -> 阴影内侧淡暗边盖住客户区，标题带下两行 242 -> 201。
   - 修法：`wl64_deco_paint64(..., phase)`，phase0（阴影 + 标题带 + 玻璃 + 标题文字 + 三按钮）在内容**之前**，
     phase1（1px 玻璃边框 + 四角圆角裁切）在内容**之后**（否则底部两圆角被内容糊掉）。
   - 证据（串口）：`[WL64] deco paint surf=1 rect=964,532,300,150 title="vimtu-peinfo" btns=3 corner=14 hdr=25 parts=1`
4. **遮挡与内容同一把尺子**
   - 窗口矩形按更高层 surface 的可见矩形切成 ≤8 块（`wl64_deco_sub64`），逐块 `fb_set_clip` 再画；
     装饰器逐像素确定 -> 不叠色不漏画。
   - 内容 blit **跳过顶部 25 行**：证据 `[WL64] blit surf=1 … src=0,0,300,150 dst=964,532 bytes=150000`
     （= 300 × 125 × 4，正好 150 − 25 行）。
5. **PE 客户端不再自绘标题栏**（`user/win/peload_crt.c`）
   - `MessageBoxA` 建面后立刻调 28 号：`[PELOAD] msgboxA decor rc=0`；
     `mb_draw` 只画底色 + 正文 + OK 按钮（外框/22px 蓝条/标题文字全部删除）。
6. **顺带修口径**：`uname` 的 release 字段由写死的 `0.1.0-vimtu64` 改成 `VIMTUOS_VERSION_STR`。
7. **文档口径回写 11 条**（详见 §8）+ `TESTS` 去重（`install_flow_test.py` 双登记删一条）。

## 3. 改动的文件

| 文件 | 改动要点 |
|---|---|
| `kernel/gui64.h` / `kernel/gui64.cpp` | 抽出装饰单一真源 `gui64_deco_frame64` / `gui64_deco_edge64` / `gui64_deco_h64`；`draw_title_buttons` 改 rect 版；`draw_window` 改为调共享装饰器 |
| `kernel/wl64.h` | ABI 28 常量与语义（`WL64_DECOR_F_WINDOW64` / `WL64_DECOR_H64`）、`wl64_surface_set_decor64` 声明 |
| `kernel/wl64.cpp` | surface 装饰字段（decor/title/打点去抖）、`wl64_deco_sub64` / `wl64_deco_paint64`（两相 + 遮挡切块）、`wl64_surface_set_decor64`、自检新增 decor 位 |
| `kernel/syscall64.cpp` / `kernel/syscall64.h` | 28 号分派 + weak 声明；`uname` release 用 `VIMTUOS_VERSION_STR` |
| `user/lib/wl.h` / `user/lib/wl.c` | 客户端包装 `wl_surface_set_decor()`（`V64_NR_WL_SURFACE_DECOR 28`） |
| `user/win/peload_crt.c` | `MB_NR_WL_DECOR 28` + 调 28 号；`mb_draw` 不再自绘标题栏/边框 |
| `tests/deco64_test.py` | **新增** 14 条断言（串口 + screendump 像素） |
| `tests/version64_test.py` | **新增** 8 条断言（发版第 1 条硬门禁：三处版本串一致，串口证据走 ring3 `cat /proc/version`） |
| `tests/status_report.py` | `TESTS` 去重 + 登记两个新脚本（104 / 104） |
| `build64.sh` | `VIMTUOS_VERSION 0.11.0 -> 0.11.1` |
| 文档 | `README.md`、`docs/混合驱动架构.md`、`docs/用户态驱动框架.md`、`docs/项目状态总览.md`、`docs/TODO_路线图.md`、`kernel/fd64.h`、`kernel/store64.h`（11 条口径回写） |

## 4. 验收（实跑 + 可复现命令）

| 脚本 | 结果 | 关键证据 |
|---|---|---|
| `tests/deco64_test.py` | **14/14 PASS** | `[WL64] decor id=1 pid=26 flags=1 mode=window hdr=25 title="vimtu-peinfo"`；`[WL64] deco paint … btns=3 corner=14 hdr=25 parts=1`；像素：标题带同色 2712/3000（90%）、旧自绘色 `0x3465A4` **0 px**、close 红底 303 px、四角严格同色 208/784（26%）vs 内部 1046/1088（96%）、标题文字深色笔画 183 px |
| `tests/peload64_test.py` | **38/38 PASS**（1 条 env SKIP：宿主 1366x768 + 1.7x 相对指针送不到 OK 按钮） | PE 六份语料全过、`reloc entry_ret=42`、`gsprobe gs_read==base`、`cui 28 导入 missing=0`、`gui blit dst=964,532`、无 PANIC |
| `tests/version64_test.py` | **8/8 PASS** | `build64.sh 0.11.1` == 内核内嵌串（`0.11.1`×5 / `VimtuOS 0.11.1`×4 / `VimtuOS Terminal v0.11.1`×1）== **客户机串口** `VimtuOS 0.11.1 x86_64`；旧串 `0.11.0` 全 0 |
| `py -3 tests/status_report.py` | 110/0/0 | 汇总行 |
| Release 回读 | **PASS** | 两个附件 `digest:sha256` 与本地 sha256 逐位相等；`v0.11.0` 的两个附件 `id/size/digest/updated_at` 与发布前快照逐条相同（未被改动） |

复现命令：
```powershell
py -3 tests\deco64_test.py          # 约 2.5 分钟（含 screendump 像素判据）
py -3 tests\peload64_test.py        # 约 7.5 分钟
py -3 tests\version64_test.py       # 约 2 分钟（发版门禁）
```

## 5. 未完成 / 阻塞

- **本版没跑完整 `status_report.py --full`**（100+ 个脚本、数小时）。因此"全量无回归"这句话**本批不成立**，
  只能说：本批改动的三条针对性验收全绿 + 上一批（跨层拖拽）的 `dragdrop64_test` / `explorer64_test`
  当时是 32/32、90/90 —— **本构建没有再复跑这两条**。
  下次开工第一件事建议：`py -3 tests\status_report.py --full`（机器要空闲）。
- **`/bin/wm`（Ring 3 合成器）这条路没接装饰**：它是有界演示（≤90s），常态桌面是内核合成路。
  要让用户态合成器也接管装饰，需要把 `gui64_deco_*` 的绘制逻辑外置成用户态库（新批次）。
- **三按钮只画不接**：最小化/最大化/关闭是像素，点击语义仍是客户区事件。
- **宿主内存环境**：`free RAM` 1.2–1.4 GB 时 QEMU（-m 512）仍然能跑完，但别同时开第二个 QEMU。

## 6. 下一步（按序，每条自带判据）

1. **全量回归**：`py -3 tests\status_report.py --full`，判据 = 0 FAIL / 0 TIMEOUT（SKIP 只允许 env 类）。
2. **装饰逻辑外置给 Ring 3 合成器**：把 `gui64_deco_frame64/edge64` 的绘制抽成用户态可用的 lib（`user/lib/deco64.*`），
   判据 = `wmfull64_test.py` 跑完后 screendump 里 PE 对话框标题带不再是 `0x3465A4` 且圆角/三按钮在，同时
   `wm64_test` 的"窗口外逐字节不变"判据不回退。
3. **三按钮接语义**：关闭 -> 销毁 surface（并发 seat 事件告诉客户端）；最小化 -> 从合成可见集合里摘掉；
   判据 = 新 `decobtn64_test.py`：点 close 后 `[WL64] surface destroy … why=client` 出现且进程退出 0。
4. **`uname` 其余字段口径**：`sysname/release` 已对，`version` 还是 `#1 ring3+syscall` 常量，可考虑并入 build tag。

## 7. 不要重做 / 不要踩的坑

1. **别把整份装饰画在内容之后**：阴影内侧暗边会压暗客户区（本批实测 242 -> 201）。必须两相。
2. **别把圆角裁切画在内容之前**：底部两个圆角会被内容糊掉。
3. **别让客户端再自绘标题栏**：那会让"接管"变成两套外观叠加（旧 22px 蓝条 + 新 24px 玻璃带）。
4. **装饰带里的内容必须跳过**（`ry + yy < WL64_DECOR_H64` 就 continue），否则 `blit bytes` 会变回 180000 且标题带被客户端像素污染。
5. **`gfx64_*` 全部走 fb clip**：`wl64_deco_paint64` 每块子矩形前必须 `fb_set_clip`，画完 `fb_reset_clip()`（否则会画到别人的窗上）。
6. **`wl64_deco_paint64` 只在 `decor & 1` 时调**：裸 surface（wmclock/wmpanel/wlclient）的合成路径一个像素都不许变。
7. **测试别写共享产物**：`build64/sysdisk.img` 必须复制副本再引导（新脚本都照这条写）。
8. **发版门禁的串口证据只能用 ring3 程序**：终端自带的 `cat`/`ver`/`uname` 是**内核 builtin**，只画终端屏幕、串口一个字没有
   （本批实测踩过：`cat /proc/version` 敲下去串口 0 新行）。正确做法 = `run /bin/busybox cat /proc/version`。

## 8. 口径过期（本批已回写，记录在此便于复查）

| 位置 | 原口径 | 现在 |
|---|---|---|
| `docs/混合驱动架构.md` | ext4 ring3 "尚未开始写 / 只有设计" | 已落地（`ext4svc.c` 974 行 + `r3drv64` 监督重启 + 写盘 1036 号） |
| 同上 §4 | 混合内核"规划中" | 已落地 |
| 同上 §5 | 内核 3,165,616 B | 3,401,760 B（v0.11.1） |
| `docs/用户态驱动框架.md:203` | "没有任何写盘接口" | `R3DRV_F_WRITE` / `1036` 已开 |
| `README.md:27/620/621/640/894` | 77 脚本 / 95 项 / tests 115 | 103→104 条登记（104 唯一）/ 110 项 / 154-155 个 py |
| `README.md` §文件系统 | ❌ 日志/快照/配额/ACL；ext4 只读 | v5 四特性齐全；ext4 读写都在 ring3 |
| `README.md` §6 | 终端"三无原则（无抗锯齿）" | v0.10.7 起抗锯齿（原文已作废说明） |
| `docs/项目状态总览.md:16-17` | 93/1/0 共 94 | 110/0/0 |
| `docs/TODO_路线图.md:71-72` | UEFI "一直是 UNVERIFIED" | 与标题对齐：v0.10.1 复验 PASS，之后每版都跑 |
| `kernel/fd64.h:43` | "没有 select/poll/epoll" | 已实现（v0.7.1/v0.7.3） |
| `kernel/store64.h:6,8-11` | `ML64_STORE_LBA=32528`、槽 A/B 8041 | 32584 / 槽 A 32584..32615、槽 B 32616..32647 |
| `kernel/syscall64.cpp:1587` | `uname` release 写死 `0.1.0-vimtu64` | `VIMTUOS_VERSION_STR` |

## 9. 环境已知问题（本机特有，别误判成代码 bug）

1. **`git push` 到 `github.com:443` 不通**（`Recv failure: Connection was reset` / `Empty reply from server` /
   `Failed to connect after 21s`），但 `api.github.com` 200、`git credential fill` 有 token。
   本批做法：`git tag` + `git push origin v0.11.1` **成功过一次**（tag 通了），随后 `main` 一直推不上去 ->
   用 `PATCH /repos/Fanqi89/VimtuOS/git/refs/heads/main {"sha": <本地 HEAD>, "force": false}` 快进（非强推，父就是远端 main）。
   脚本留档：`$env:PI_SCRATCH_DIR\rel_v0111.py`（含 Release 建立/上传/双向核对/旧版附件对照）。
   下次先试一次直连 `git push`；不通再走 API。
2. **内存争抢**：宿主 8 GB，另一个会话的 QEMU 会把我方 QEMU 挤死（`rc=4294967295`，串口停在正常 kheart、无 PANIC）。
   开工前看 `Get-CimInstance Win32_OperatingSystem` 的 FreePhysicalMemory 与 `Get-Process qemu-system-x86_64`。
3. **工具链路径**：构建必须用 `C:\msys64\usr\bin\bash.exe`，且脚本里要 `export PATH=/usr/bin:/bin:...` 再调 `bash build64.sh`
   —— 否则内层 `bash` 会解析到 WSL 的 `C:\Windows\System32\bash.exe`（实测报 "用于 Linux 的 Windows 子系统没有已安装的分发"）。
4. **screendump 与串口的延迟**：对话框 idle 超时 8 s，抓帧要"看到 `deco paint` 就立刻打"，本轮两次都成功（1.0 s 后抓）。
5. **未跟踪**：`docs/images/GUI.png`、`docs/images/GUI2.png`（用户截图，本批**没有**提交）。
