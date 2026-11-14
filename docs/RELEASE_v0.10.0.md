# VimtuOS v0.10.0 —— 三生态里程碑版本

> 从 v0.9.41 直接跨越到 **v0.10.0**。理由：这不是打补丁，是**底层阻断级回归被连根拔掉**
> 加上**三生态（原生 / Linux / Windows）全链路首次同时跑通**。
> 本轮所有结论都来自**实测日志**，没有实测的一律标注。

---

## 🏆 三大核心突破

### 一、AHCI 挂载彻底修复 —— 清除"写死盘号"（VMware / 真机级）

**症状**（用户现场）：VMware 虚拟盘（AHCI）+ ISO 全新安装到 5 GB 主分区 → 安装显示成功 →
重启后 `[VFS64] mount_system FAILED (mount)` + `[WL64] wm skipped (no system volume)`，
壁纸与开始菜单图标全空。

**根因是一族"把盘号/路径写死"的回归，共三处**：

| # | 位置 | 写死的东西 | 后果 |
| --- | --- | --- | --- |
| 1 | `kernel/app64.cpp` `app64_main_part_lba64()` | `if (drive >= 0 && drive < 4)` 只认 PATA | AHCI 盘号是 **8..15**，永远返回编译期常量 `PART_MAIN_LBA`，主分区起点一变就挂错 LBA |
| 2 | `kernel/wl64.cpp`（demo / wm 两处） | `vfs64_mount_system64(0, ...)` 写死 **drive 0** | AHCI-only 机器上 drive 0 是空的 PATA 槽 → 合成器必然跳过 |
| 3 | `kernel/wl64.cpp` 合成器路径 | 契约 `/bin/wm.elf`，而构建落在 `/lib/wm.elf` | 合成器 ELF 找不到 → 桌面没有窗口层 |

**修法（都是"不再猜"）**：

1. 任何 drive 都去读**它自己的 MBR** 0x07 项，读不到才退回常量；
2. 挂系统卷改成"drive 0 先试 → 失败按盘遍历（PATA/AHCI/NVMe/USB 统一口径）"，成功时打印
   `[WL64] system volume drive=<n> scanned=1`；
3. 合成器路径 `/bin/wm.elf` → 不在卷里就回退 `/lib/wm.elf`，并打印实际用的那个。

**VMware 端到端实测（全新 5 GiB SATA 盘 → IDE 光驱 ISO 安装 → 关机 → 单盘重启）**：

```
[PART] 新建分区表 OK drive=8 boot=65+32519 main=32584+10354839 esp=10387423+98304
[INSTALL] 完成：已写 57343 扇区 / volume grow ok total=10354839 (5056 MiB)

[APP64] main partition drive=8 lba=32584 (from MBR type=0x07)
[VFS64] mount_system slot=0 drive=8 start=32584
[VFS64] mount ok blocks=10354839 inodes=2048 free=10335067 version=5
[ICON64] init pack entries=110 icons=30 bad=0 ok=1 src=vfs path=/etc/iconpack.bin
[WL64] wm path fallback=/lib/wm.elf
[WL64] composer pid=25 seat=1 backend=soft-lfb rect=0,0,1024,768
[WM]   composer seat=1 gpu=0 region=0,0,1024,768
```

**A/B 对照（可复现）**：把已知可用的系统盘整体搬到 LBA 40960（卷逐字节完好）后
——修前 `bad drive, fallback 32584` → `mount_system FAILED`；修后 `drive=8 lba=40960 (from MBR)`
→ `mount_system slot=0 drive=8 start=40960`。

**同时补上的可观测性**（这批 bug 之所以查了很久，是因为失败点是静默的）：

* `[PROC64] kill` 增加 **`caller=<返回地址>`** —— 一次运行就指名了那条困扰两个版本的
  "~16 s SIGKILL"，真身是 `term_run_redir64()` 的 120 tick-秒等待超时（TCG 下 tick 比墙钟快好几倍）；
  现在它**只撒手、不杀进程**（`[TERM] run detach pid=..`），长驻程序不再被拦腰砍死。
* `[AHCI64] xfer SKIP reason=…`（7 处静默失败点）；
* `[ATA64] read enter … drives=/ahci_base=/rprobe=1`。

### 二、UEFI + BIOS 双引导底座加固（如实标注）

* **BIOS 路径：本轮 VMware 实测全绿**（El Torito → `LOADER64.BIN` → `KERNEL64.BIN` → 桌面）；
* **UEFI 所需的分区与文件：本轮实测已正确写入**——
  `[INSTALL] esp files: BOOTX64.EFI=2560B UEFI64.BIN=40960B KERNEL64.BIN=15728640B`、
  `[INSTALL] gpt written (main + esp), pmbr ok … backup_hdr_lba=10485759`，
  宿主侧解析：`P1 0xEF 65+32519 / P2 0x07 32584+10354839 / P3 0xEF 10387423+98304` + `55AA` 正确。
* **UEFI 固件引导本身本轮未复验**（本轮没有起 UEFI 固件的 VM）—— 标注为 **UNVERIFIED(env)**，
  不当作已验证的成果宣称。

### 三、Windows PE GUI 弹窗的鼠标交互落地

`user/win/peload.c` + `peload_crt.c`（自研 Win64 PE 加载器）在 v0.9.46/0.9.47 打通
`MessageBoxA` → wl_surface 真弹窗之后，本版本把**鼠标拖动窗口**与**真点击 OK** 全部跑通：

```
[WM] place id=1 pos=490,325 mode=center
[WL64] move id=1 pid=25 pos=490,325 from=964,532 by=composer
[WM] drag begin id=1 grab=140,10 from=490,325
[WM] drag move  id=1 pos=250,185 dx=-240 dy=-140
[WM] drag end   id=1 pos=250,185 moved=N
[WM] route surf=1 pid=26 type=5 x=400 y=313 sx=150 sy=128
[PELOAD] msgboxA close reason=clicked
[PROC64] exit pid=25 code=0
```

像素核验（screendump，VM 关掉后离线算）：

* 居中：标题栏 #3465A4 = **4,533 px**、正文底 #F2F2F2 = **28,499 px**、黑字 349 px、
  左边 60 px 外 **0 px**、旧面板槽位 (964,532) 上 **0 px**；
* 拖动后：新位置 (250,185) 标题栏 4,533 px、正文 28,499 px，**旧位置拖影 0 px**；
* 点击后：全屏标题栏色 **0 px**。

`tests/wmfull64_test.py`：**41 PASS / 0 FAIL / 0 SKIP**（起点 27/14）。

---

## 其他修复

* **`term_run_redir64` 不再 SIGKILL 长驻程序**（见上），并修掉"进程已消失时会去 kill 一个
  可能已被复用的 pid"的隐患。
* **MessageBoxA 超时改成"空闲超时"**：`MB_TIMEOUT_MS 8000` 仍是默认，但语义是
  **多久没有 seat 事件**（交互即续命，硬上限 180 s）；可选 `argv[2]` 覆盖
  （`run /bin/peload <exe> 90000`）。
* **`[WM] route` 日志预算拆开**：`route surf=` 单独 240 行（否则取证行被 `route none` 吃光）。
* **合成器寿命 `WM_SEEN_MS` 60 s → 30 min**：合成器是长驻服务，不该在窗口还在屏上时自裁。
* **`[UI] perf` 预算 180 s → 1800 s**：它是夹具拿光标绝对坐标的唯一实时手段。
* **输入队列 ev64 每队列锁**（IRQ 与任务两个上下文交叉会改坏 head/count）。
* **`elfrun bg <path>`**：长驻服务不再被 120 s 等待上限收掉。

## 已知边界（如实）

* **ISO 必须用 IDE 光驱引导**：本内核 AHCI 侧的 ATAPI **只识别、不驱动**
  （`[AHCI64] atapi read FAILED … cmd timeout`）。挂 SATA 光驱时 ISO 会卡在 `L:media=cd` 之后。
  VMware 默认给这类 guest 的就是 IDE 光驱。
* **AHCI 没有 FLUSH CACHE EXT**（`kernel/ahci64.h` 自述："落盘时机交给控制器缓存"）。
  这不是本次问题的病因（本次失败可 100% 确定性复现、无需断电），但"写断电也不丢"目前**不成立**，
  已列入下一批。
* 外壳 GUI 主循环在有窗口时约 1–1.6 Hz（TCG + 整屏重绘）；这是性能问题，不是死机。
* `tests/wm64_test.py` 仍 2 条红（⑦ handback 的 `px=0` 与"窗口外逐字节不变"），与本版无回归关系，
  是上一批新升级断言首次真机跑留下的，待单独定位。

## 验收汇总（本版本实测）

| 测试 | 结果 |
| --- | --- |
| `tests/wmfull64_test.py` | **41 / 0 / 0** PASS/FAIL/SKIP |
| `tests/peload64_test.py` | **38 / 0 / 1 SKIP(env)** |
| `tests/wl64_test.py` | **91 / 91** |
| `tests/install_persist64_test.py` | **13 / 13** |
| VMware SATA+ISO 端到端（`vmw_ahci_iso.py`） | 安装 `volume grow ok total=10354839`，重启 `mount_system drive=8` |
| VMware 桌面（`vmw_desktop.py`） | `mount_system drive=8 : OK` / `desktop/composer : OK` |

## 镜像

* `vimtu64-64.iso` —— BIOS El Torito + UEFI（ESP）+ GPT hybrid MBR
* `vimtu64-64.img` —— 同上（可直写 U 盘/磁盘）
