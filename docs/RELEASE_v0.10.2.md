# VimtuOS v0.10.2 —— 真机加固 + 桌面帧预算（大页落地，帧率目标未达成）

发布日期：2026-11-14
上一版：[v0.10.1（查漏补缺）](RELEASE_v0.10.1.md)

> **一句话**：这一版修掉了一个**会让 UEFI 安装出来的盘直接黑屏**的阻断级缺陷（真机可测），
> 把拖动窗口的帧率从 5.7 fps 提到 17.5~19.6 fps，并把 shm / 帧缓冲的映射改成了 2 MiB 大页
> （日志可证）；**但 60 fps 没做到**，卡点已用数据定位到一个新问题（见 §4），如实记录。

---

## 1. 阻断级修复：UEFI 只拷 4.13 MB 载荷（真机黑屏）

`boot/efi/uefi64.c` 把载荷字节数写死成 `8073*512`（4.13 MB，v0.5.18 时代的 `system.img` 大小），
而它现在是 **57,343 扇区（29.3 MB）**：

* `load_file()` 只读前 4.13 MB，尾部清零；
* 介质描述符也只报 8073 扇区 → 安装程序只拷 4.13 MB；
* 结果：UEFI 装出来的盘 **主分区（LBA 32584 起）整块是零**，装完重启黑屏。

**旧测试为什么一直绿**：`uefi64_install_test` 只比 `LBA1..64`（loader）与 `LBA65..`（kernel），
两者都在前 4.13 MB 内 —— 截断它看不出来。这是**判据漏洞**，本版补上。

修法：按文件真实大小读（`SetPosition(END_OF_FILE)` + `GetPosition`），上限变显式检查
（内核 46 MB / 载荷 32 MB），介质描述符按真实扇区数上报，`BootInfo.kernel_size` 也是真值。

## 2. 真机不再"黑屏且没有一句提示"

`uefi64.c` 所有失败路径以前只写 COM1，真机没串口 = 纯黑屏无原因。本版加固件 `ConOut` 输出：

```
U:no GOP (this machine has no usable linear framebuffer)
U:image too big for the boot staging window
U:KERNEL64.BIN missing or unreadable
U:size SYSTEM.IMG = <真实字节数>        U:payload sectors=<ceil(bytes/512)>
```

另外两处真机条件级缺陷：

* **MMIO BAR ≥ 4 GiB 闸门**（`nvme64` / `ahci64`）：真机 M.2 NVMe 的 BAR 常在 Above-4G，而内核只
  恒等映射 0..4 GiB —— 以前第一条寄存器读就是野地址。现在
  `[NVME64] skip reason=bar0-above-4g` / `[AHCI64] skip reason=abar-above-4g`，拒设备不拒整机。
* **GOP 原生分辨率 > 后备缓冲**（`fb_init`）：`backbuf_storage` 是 3840x2160，以前不钳制，
  4096x2160 / 5120x2880 时第一次清零就写穿 `.bss`。现在钳到上限并走既有缩放路径，
  打 `[FB64] geom clamp lfb=… render=… zoom=…`。

## 3. 桌面帧预算第一刀：5.7 → 17.5~19.6 fps

取证（`tests` 侧新建 `p1_wmperf.py` 夹具，逐行串口到达时间戳）发现：拖动时**每个鼠标事件打 11 行
串口日志**，而每行 ≈1.4 ms（COM1 每字符一次 VM exit）—— 单帧 175 ms 里约 170 ms 是打点。

修法：全部热路径打点改**预算**（前 N 条原文照打、之后稀疏采样，验收要 grep 的行照旧出现）；
去掉输入批次后的重复整窗重画；背景回填按行整段拷贝、blit 免除法、内核提交路径改行级
`memcpy_64()`；加 **60 Hz 帧节拍**（16 ms，PIT 4 ms 粒度下最接近）与客人侧时间戳 `t_ms=`。

## 4. 2 MiB 大页：映射落地并验证，但**判据没达标**

内核新增（全部有日志可证）：

```
[FB64] backbuf pa=0x0000000000A00000 huge=1
[FB64] map huge pid=25 blocks=1 va=0x0000000140000000 pa=0x0000000000A00000
[SHM64] create pid=25 id=5 size=2097152 pages=512 huge=1 pa=0x000000000A200000
[SHM64] map   pid=25 id=5 slot=0 va=0x000000013E800000 pa=0x000000000A200000 pages=512 huge=1 u=1
[SHM64] create pid=25 id=6 size=1998848 pages=488 huge=1 pa=0x000000000A400000
[SHM64] map   pid=25 id=6 slot=1 va=0x000000013EA00000 pa=0x000000000A400000 pages=488 huge=1 u=1
```

* `page_alloc_2mb_64()`：按 2 MiB 对齐扫描位图，取 512 页连续块（`kernel/mem64.cpp`）；
* `user64_map_phys_2mb64()`：直接写 **PD 项**（PS=1，不建 PT），va/phys 必须双对齐且在 4 GiB 直映内
  （`kernel/usermode64.cpp`）；页级查询助手（`is_user` / `page_flags` / `unmap_page`）同步认识大页；
* shm ≥1 MiB 的对象走 2 MiB 块、整块 512 页填满；fb 后备缓冲 2 MiB 对齐 + 大页映射。

**验收判据（`[WM] bench`，8 次取最小以排除抢占，`cks=` 非 0 证明没被编译器优化掉）：**

| | 目标 | 实测 |
|---|---|---|
| `bgfull_us`（整屏 4 MiB 快照回填） | **< 20,000 µs** | **460,538 ~ 501,288 µs（未达标）** |
| `copy4k_us_min` | — | 4 µs（4 KiB 拷贝本身正常） |
| 拖动帧率 | 60 fps | **17.5 ~ 19.6 fps**（改前 5.7） |

**为什么没达标（新证据，`[WM] bench scale`，快照→帧缓冲）：**

```
us_4k=68   us_64k=1110   us_256k=4726   us_1m=108493   us_3m=389189
     ↑ 线性（17~18 ns/byte）        ↑ 断崖（4x 尺寸 23x 时间）   ↑ 103 ns/byte
```

大页把页表项从 1000 个压到 2 个，**拷贝成本几乎没变** → 不是 TLB/页表问题；
而"写自己的 `.bss`"是 1 ns/byte、"写共享帧缓冲"是 17~103 ns/byte，并在 ~512 KiB 处出现断崖。
下一步先查这个（详见 `docs/桌面帧预算.md` §5.3 / §7），再谈 60 fps。

## 4.1 发版口径：大页代码就位，但两个开关默认关闭

`SHM64_HUGE_ENABLE = 0` / `FB64_HUGE_ENABLE = 0`（`kernel/proc64.cpp` / `kernel/syscall64.cpp`）。
打开它们时 `wm64_test` 出现像素级回归（⑤ 点击后开关读回 `0x000000`；⑦ 交回时 `erased=13672`）。
⑦ 随 fb 大页关闭转绿 → ⑦ 是大页的锅；⑤ 最终确认是**启动基准自己**在首帧前涂了屏
（现已由 `/etc/wm_bench` 探针文件触发，发版路径不跑），修完 `wm64_test` **89/89**。

所以本版交付的是：**能随时打开的大页机制 + 一条明确的重新打开判据**，而不是一个没验证的加速。

## 5. 判据（本版实测，全部实跑）

| 测试 | 结果 |
|---|---|
| 全量构建 `build64.sh` | **EXIT=0**；内核 3,335,024 B；`/lib/wm.elf` 27,560 B |
| `tests/wm64_test.py` | **89 / 89 PASS**（判据升级：一帧成本 = `us + bg_us`，并断言 `t_ms=` 存在） |
| `tests/wmfull64_test.py` | **41 / 41 PASS** |
| `tests/uefi64_install_test.py` | **PASS**（判据加强：`U:size` / `U:payload sectors` / 主分区起点·中段·盘尾逐字节 / 目标盘大小） |
| `tests/drv64_test.py` | **54 / 54 PASS** |

> 全部实跑；`flush64_test` / `install_persist64_test` / `ahci64_test` / `power64_test` 本版**未跑**（如实标注）。

## 6. 真机首测要看的三行

```
U:size SYSTEM.IMG = …            U:payload sectors=…
U:bootinfo lfb=… lfb64=… src=gop hi=0|1
[FB64] backbuf pa=0x… huge=1     [FB64] map huge … blocks=…
[WM] bench bgfull_px=… bgfull_us_min=… copy4k_us_min=…
[WM] bench scale us_4k=… us_64k=… us_256k=… us_1m=… us_3m=…
```

真机上 `bgfull_us_min` 会明显好于 QEMU TCG（QEMU 的 TCG 没有真实缓存层次），但**这不等于达标**，
要在真机上重新量一遍才算数（UNVERIFIED 到那时为止）。

## 7. 已知未做

* 60 fps 未达成（见 §4/§5）。
* dpkg 的 zstd 解码 / symlink 策略 / 4-8 MiB 上限、ext4 写入引擎 —— 审计完成、缺口清单在
  `docs/TODO_路线图.md` §8/§9，**本版未实现**。
* AHCI 侧 ATAPI 光驱读（ISO 安装仍需 IDE 光驱）、USB 存储 FLUSH —— 与上一版相同。
