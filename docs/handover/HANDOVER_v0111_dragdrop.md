# HANDOVER —— 跨层拖拽（explorer → 桌面）落地；PE 窗口统一装饰未动，未发版

- 日期：2026-11-22 19:10（本地）
- 版本串：`build64.sh` `VIMTUOS_VERSION="0.11.0"` —— **本批未 bump**（按用户顺序：PE 装饰做完才发 v0.11.1）
- 分支/HEAD：`main` @ `113d37c6`（v0.11.1-wip 第二步·跨层拖拽（explorer -> 桌面））
  - 上一笔：`88f3dcba` v0.11.1-wip: 资源管理器统一 UI（清掉直角白底旧菜单）+ 新建文本文件
- tag：**没有** v0.11.1（最新 tag 仍是 `v0.11.0`）；Release：未建
  - 本单所在提交：`git log -1 --format=%h -- HANDOVER_v0111_dragdrop.md`（不写死 hash，避免 amend 后失准）
- 远端：未推送（`git status -sb` 自查；本批提交在本地）

## 1. 现场（下个会话必须核对）

| 项 | 值（2026-11-22 19:10 实测） |
|---|---|
| 系统内核 `build64/kernel64_os.bin` | **3,398,240 B**（闸门 15,728,640 B = 30,720 扇区） |
| 安装内核 `build64/kernel64.bin` | 1,899,136 B |
| `build64/loader64.bin` | 4,090 B（容量 32 KB） |
| `vimtu64-64.iso` | 93,425,664 B |
| `vimtu64-64-disk.img` / `build64/sysdisk.img` | 38,456,320 B / 38,456,320 B |
| 构建 | `C:\msys64\usr\bin\bash.exe -lc "cd /c/Users/fanqi/Desktop/VimtuOS/Vimtu64 && bash build64.sh"` → **EXIT=0** |
| 能力清单 | `py -3 tests/status_report.py` → **完成 110 / 部分 0 / 未做 0** |
| 测试规模 | `tests/*.py` = **153**（其中 `*_test.py` 144）；`status_report.py` 的 `TESTS` = **103 条登记 / 102 唯一**（`install_flow_test.py` 重复登记，待删一条） |

## 2. 本批做了什么（每条带证据）

1. **拖拽状态机**（explorer 条目 → 桌面）
   - 阈值 `EXP_DRAG_MIN 8`px；按下装填拖拽源、双击成立即作废（`kernel/explorer64.cpp`）
   - 证据：`[UI] explorer drag begin idx=1 name=etc dir=1 sx=417 sy=178 min=8`
2. **拖拽幽灵**（圆角底板 + 条目图标 + 文件名 + 可放/不可放提示）
   - 画在 `render()` 里**所有窗口之上、光标之下**：`kernel/gui64.cpp` 的 `explorer64_drag_paint64()` 调用点
   - 脏矩形：`exp_drag_dirty()` 先擦旧位置再标新位置（复用 `gui64_dirty`）
3. **落点判定 + 桌面建项**
   - 落点 = 不在任何窗口 / Dock / 开始菜单上（`gui64_hit_window64()` 是本批新增的薄封装）
   - 证据：`[UI] explorer drag drop path=/etc dir=1 x=1098 y=125 target=desktop ok=1`
     → `[DESK64] file add idx=3 kind=9 dir=1 vol=67 path=/etc x=1080 y=108 total=4 why=explorer-drag`
4. **`/etc/desktop.links` 真落盘**（`<dir>|<letter>|<x>|<y>|<path>`）
   - 证据：`[DESK64] links save why=add n=1 bytes=18 rc=18 ok=1`（**rc == bytes** 才算成功）
   - 写盘失败**回滚图标**：`[DESK64] file add rollback reason=links-write-failed`
   - 卷字母必须存（C:/a.txt 与 D:/a.txt 是两个文件），去重键 = (letter, path)
5. **桌面双击文件图标** → `explorer64_open_path64(path, letter)`：换盘先切卷，再定位并选中
6. **修掉本批自踩的真回归**：`[DESK64] selftest FAIL mask=28`
   - 根因：我把"保留文件项"塞进了 `apply_set()`，而 selftest 判据是"apply_set("0,1,2|") 之后必须正好 3 项"
   - 修法：`apply_set` 保持纯净（只认 kind 串）；文件项由调用方 `items_keep_files64/items_restore_files64` 接住（`desktopops64_tick64` 与 `restore_defaults`）
   - 证据：修复后 `[DESK64] selftest PASS mask=0 items=4`

## 3. 改动的文件

| 文件 | 改动要点 |
|---|---|
| `kernel/desktopops64.h` | 新增 `DESKOPS_KIND_FILE=9`；`MAX_ITEMS 8→16`；`LINK_MAX 8`、`LINK_PATH_MAX 128`；文件快捷方式 API |
| `kernel/desktopops64.cpp` | `/etc/desktop.links` 读写、`add/remove/letter` 访问器、托盘项绘制、回收站/右键菜单分支、写盘回滚、`tick64`/`restore_defaults` 保留文件项 |
| `kernel/explorer64.h` | 导出拖拽三函数 + `open_path` |
| `kernel/explorer64.cpp` | 拖拽状态机、幽灵绘制、落点判定、按路径打开、关窗/开窗清状态 |
| `kernel/gui64.h` | `gui64_hit_window64()` 声明 |
| `kernel/gui64.cpp` | 落点命中封装、render 画幽灵、桌面双击文件图标 |
| `tests/dragdrop64_test.py` | **新增**端到端验收（拖拽/反例/跨重启持久化） |
| `tests/status_report.py` | `TESTS` 门禁追加 `dragdrop64_test.py` |

## 4. 验收（实跑）

| 脚本 | 结果 | 关键证据 |
|---|---|---|
| `tests/dragdrop64_test.py` | **32/32 PASS**（对上一构建，见 §5） | `drag begin … min=8` / `target=desktop ok=1` / `file add kind=9` / `links save rc=18 ok=1` / 重启后 `links load n=1 skipped=0 ok=1` + `[DESK64] item idx=3 kind=9 name=etc` |
| `tests/explorer64_test.py` | **90/90 PASS**（同上构建） | `=== RESULT: PASS === checks=90 ok=90` |
| `py -3 tests/status_report.py` | 110/0/0 | 汇总行 |
| `bash build64.sh`（最终构建） | EXIT=0 | 内核 3,398,240 B |

复现命令：
```powershell
py -3 tests/dragdrop64_test.py --keep      # 两次开机，约 8 分钟（QEMU TCG）
py -3 tests/explorer64_test.py --keep      # 90 条断言，约 9 分钟
```

## 5. 未完成 / 阻塞（**是环境问题，不是代码问题**）

- **最终构建的 GUI 复验没跑成**（阻塞）：本机同时有另一个会话在跑 SutureOS 的 GUI 扫描
  （`scripts/gui_sweep.py` 等，QEMU 512 MB × 多个），宿主物理内存只剩 **1.2–1.8 GB / 7.9 GB**，
  我的 QEMU 在客户机健康的情况下被杀（`proc.returncode = 4294967295`，串口停在正常
  `[TASK64] kheart beat=N`、**无 PANIC / 无 TRIPLE FAULT**）。
- 因此：**32/32 与 90/90 是在"最终提交前一次构建"上取得的**；最终构建（含两处小硬化：
  卷字母参与去重、`links_save64` 返回状态并回滚）只做了**编译 + 全量链接 + 自检行**验证，**未再跑一整套 GUI 验收**。
  阻塞解除条件：宿主 free RAM > 3 GB 且无其它 qemu 进程。
- 复跑验收（机器空闲时）：
  `py -3 tests/dragdrop64_test.py` —— 期望 32/32。

## 6. 下一步（按序，每条自带判据）

1. **（用户排的第二刀）`wl_surface → Window`：合成器统一接管 PE/ring3 窗口的圆角与标题栏**
   - 判据：`run /bin/peload /pexcorpus/gui.exe` 弹出的 300×150 对话框由 `draw_window()` 画装饰；
     像素上圆角处出现 `gfx64_corner_cut64` 的裁切（四角背景色恢复）、标题栏三按钮可见；
     `tests/peload64_test.py` 38/38 不回退；`tests/wm64_test.py` / `wmfull64_test.py` 的"窗口外逐字节不变"判据不回退。
2. **文档口径回写**（发版前必做，见 §8）：`docs/混合驱动架构.md` §3/§4/§5、README §6/§文件系统、`docs/TODO_路线图.md`。
3. **`TESTS` 去重**：`install_flow_test.py` 重复登记两次，删一条（103 → 102）。
4. **发版 v0.11.1**（等第 1 条做完）：`VIMTUOS_VERSION 0.11.0→0.11.1` → 重建 → `status_report --full` → 串口核对
   `[SET64] about version=VimtuOS 0.11.1` + `VimtuOS Terminal v0.11.1`（旧串 0 次）→ tag `v0.11.1` → Release（旧 Release 一字不动）。

## 7. 不要重做 / 不要踩的坑

1. **别把"保留文件项"塞回 `apply_set()`** —— 会立刻打红 `[DESK64] selftest FAIL mask=28`（本批亲身踩过）。
2. **别在 `links_save64` 之外单独写 `/etc/desktop.links`**：它是整文件重写，必须一次成型（部分写会丢条目）。
3. **拖拽落点不要用"光标下没有窗口"以外的新判据**（Dock/开始菜单/任务栏行都要排除，否则会把图标放到 Dock 上）。
4. **`g_ents[]` 下标在拖拽期间是快照**：目录不会自动刷新，所以按项取路径是安全的；若将来加了自动刷新，必须改成 arm 时快照路径。
5. **测试别写共享产物**：`build64/sysdisk.img` / `system.img` 必须复制副本再引导（19 个脚本的注释都写了这条）。
6. **QEMU 端口会被同机其它项目的 QEMU 抢**（对方用 `-qmp tcp:127.0.0.1:<随机>`）：夹具要先探 monitor 可达再开测，
   起不来就换端口重试（`tests/dragdrop64_test.py` 里的 `Vm`/`Mon` 已经是这个写法，可复用）。

## 8. 口径过期（文档 vs 代码，发版前必须回写）

| 位置 | 文档怎么说 | 实况 |
|---|---|---|
| `docs/混合驱动架构.md:95-96,121` | ext4 ring3 服务"尚未开始写 / 只有设计没有代码" | `user/drivers/ext4svc.c` 974 行 + `kernel/r3drv_abi.h` 三条 op；`ext4rpc64` 12/12 |
| `docs/混合驱动架构.md:104-105` | 混合内核"规划中" | `kernel/r3drv64.cpp` 已有 `service died` / `restart attempt` / 只读回退 |
| `docs/混合驱动架构.md:125` | 内核 3,165,616 B（v0.9.31） | **3,398,240 B** |
| `docs/用户态驱动框架.md:203` | r3drv"没有任何写盘接口" | `R3DRV_F_WRITE` + `R3DRV_NR_WRITE 1036` + `ext4write64` 16/16 |
| `README.md:614-615,634,888` | 95 项能力 / 77 脚本 / tests 115 | **110 项 / 103 条（102 唯一）/ 153 个 py** |
| `README.md:383` | ❌ 日志/快照/配额/ACL | v5 四特性都在（`journal64` 20、`quota64` 15、`acl64` 13、`snapshot64` 16） |
| `README.md` §6 | 终端"三无原则（无抗锯齿）" | v0.10.7 起走 AA；`font.cpp:1238-1241` 直写混合，mono setter 实际未被调用 |
| `docs/项目状态总览.md:16-17` | 93/1/0 共 94 项 | 110/0/0（同文 :357/:458 已写 110，自相矛盾） |
| `docs/TODO_路线图.md:69 vs :71-72` | 标题"已复验 PASS"，正文"一直 UNVERIFIED" | 同一节自相矛盾 |
| `kernel/fd64.h:43,45` | "无 select/poll/epoll" | `fd64.cpp:220-231` 有 epoll |
| `kernel/store64.h:6` | `ML64_STORE_LBA=32528` | 实际 **32584**（`memlayout64.h:91`） |

## 9. 环境已知问题（本机特有，别误判成代码 bug）

1. **内存争抢**：宿主 8 GB，另一会话常驻 1–2 个 QEMU → free RAM 1.2–1.8 GB 时我的 QEMU 会被杀（rc=-1）。
   开工前先看：`Get-CimInstance Win32_OperatingSystem | % { $_.FreePhysicalMemory/1MB }` 与 `Get-Process qemu-system-x86_64`。
2. **端口争抢**：对方 QEMU 用随机 `-qmp`/`-monitor` 端口；`fst.free_port()` 有竞态 → 夹具要重试（见 §7.6）。
3. **工具链路径**：构建必须用 `C:\msys64\usr\bin\bash.exe`（PATH 上的 `bash` 是 WSL）；QEMU 在
   `C:\Program Files\qemu`（不在 PATH，`tests/proc64_test.py:find_qemu` 会自己找）；`nasm` 在 msys2。
4. **telnet monitor 偶发 ConnectionRefused**：客户机还活着时也会发生 → 只做有界重连，不改判据。
