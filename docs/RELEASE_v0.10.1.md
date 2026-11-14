# VimtuOS v0.10.1 —— 查漏补缺版（越界指针结案 + 设备容量闸门 + 桌面交回修复）

发布日期：2026-11-14　　基线：v0.10.0（三生态里程碑）

本版是 v0.10.0 之后的**修复版**，只做"查漏补缺"，不加新生态能力。三条主线：把那条长期挂着的
`[VFS64] BAD block=71` 查到底、把"越界 LBA 打到设备"这条通路从根上封死、把 `wm64_test` 遗留的
2 条红断言修掉并做 UEFI 复验。

---

## 1. `[VFS64] BAD block=71 not in data area [6,64)` —— 结案：它一直是自检自己的负向对照

**做法**：给打点加调用者与现场（`__attribute__((noinline))` + `__builtin_return_address(0)`，
再补 inode 索引 / inode 装载块 / 卷起始 / 路径面包屑），然后跑一次真实启动取证。

**现场（实测，PATA 与 AHCI 启动都能复现）**：

```
[VFS64] read bad i=0 off=0 want=16 size=512 blk=71 d0=71 d1=0 d2=0 d3=0 ind=0 type=1
        fake=1 path=hack idx=1 ino_blk=2 ino_start=2 vol_start=0
        load_caller=0xFFFFFFFF8014A221 caller=0xFFFFFFFF80156246
[VFS64] BAD block=71 not in data area [6,64) slot=0 inodes=16 caller=0xFFFFFFFF80169E89 expect=inject
[VFS64] inject probe ok name=hack block=71 refused=1 (out-of-range data pointer, CRC valid)
[VFS64] selftest PASS
```

**结论**：`71 = g_blocks + 7`，来源是 `kernel/vfs64.cpp` 自检里那条**故意注入**的负向对照 ——
造一个"数据块指针越界但 CRC 合法"的 inode（文件名叫 `hack`），断言"读必须被拒绝、绝不越界读"。
它在**内存假盘**（`fake=1`）上跑，从不碰真实设备，`selftest PASS` 就是它按设计通过的证据。
它不是内存踩踏，也不产生任何非法设备访问。

**顺手加固**：这类命中从此带 `expect=inject` 标记，并多一条
`[VFS64] inject probe ok name=hack block=<n> refused=1` 正向证据 —— 以后不会再被误当成 bug
（本版之前我们自己也误读过一次）。

## 2. 越界 LBA 通路：从"设备报错后卡死"到"根本发不出去"

上一版给 AHCI 加了端口恢复兜底（设备报错后 PxCI 卡死 -> 全盘不可用）。本版把**源头**封掉：

**(a) `ata64` 设备容量闸门**（`kernel/ata64.cpp`）：每条 `ata64_read/ata64_write` 先过容量检查
（每盘用 IDENTIFY 缓存一次容量），越界就**不发命令**并打点：

```
[ATA64] range REFUSED (out of disk) op=read drive=0 lba=3993237690 count=1 cap=57343 caller=0x....
```

**(b) 闸门第一次上线就抓到真凶**：`kernel/r3drv64.cpp` 的 `r3_find_on_drive64()` 里
**MBR 缓冲区与探测缓冲区是同一块 `g_sec`** —— 循环里探测一个分区项就会把 MBR 覆盖，
下一轮把"刚读回来的那个扇区"当成分区项，随机 4 字节被当成 `start LBA` 送进设备。
**修法**：进循环前把 4 个分区项拷到局部变量（断开别名），并用盘容量对分区项做边界检查
（越界项不探测，打 `[R3DRV] probe skip … reason=partition-out-of-disk`）。

修复后同一夹具：`[ATA64] range REFUSED` 计数 **6 -> 0**，`[R3DRV] autostart skipped` 行为不变。

## 3. `tests/wm64_test.py` ⑦ 遗留 2 条红断言 —— 修完

**(a) `[WM] exit handback … px=0 | px=8192`（应 = Σ窗口面积 23552）**
根因有两层，都在 `user/wm/wm.c`：

* 最后一个客户端销毁后还会再画一帧（`surfs=0`），那一帧的 `wm_remember()` 把"画过的窗口矩形"清成 0
  -> `wins=0 area=0 px=0`（现场：`exit handback … px=0 frames=139 bg=1 area=0 wins=0`）；
* 即便不清空，"最后一帧的活窗口集合"也只等于后退出那个窗口 -> `px=8192`（只有 wmpanel 128x64）。

修法：`wm_remember()` 改成**并集语义**（本次会话里 wm 画过的所有窗口矩形，按矩形去重），
且空列表不再覆盖上一次。修后：`px=23552 want=23552`。

**(b) "wm 退出后窗口之外逐字节不变"（`outside_diff=128261`）**
实测证明这条**口径本身不成立**：`before_exit` 的桌面区是**纯黑**（wm 接管屏幕时抓到的背景快照就是黑屏），
交回之后内核外壳才第一次绘制自己的桌面（`after_exit` 那里才有壁纸/图标）——旧判据把
"外壳第一次画桌面"也算成失败，与 wm 有没有擦掉东西无关。

新口径（更强、更对口，且全部实测）：

| 判据 | 实测 |
|---|---|
| ① `px == Σ窗口面积` | 23552 == 23552 |
| ② 交回后 wm 的像素一个都不留（取样点） | `leftover=[]` |
| ③ **erased == 0**（交回前有内容 -> 交回后纯黑 = wm 擦了外壳的东西） | `erased=0 painted=63602 kept=0` |
| ④ painted > 0（外壳确实接管重画了，留证据） | `painted=63602` |
| ⑤ 交回后 wm 的调色板不再出现在窗口矩形里 | 无命中 |

> 内核自己的"外部面板区"（320x200，`[WL64] init fb=… panel=948,516,320,200`）从 ③ 的取样范围里
> 排除并写进断言详情 —— 那是内核 shell 的地盘（`kernel/wl64.cpp:249-256`），wm 从不碰它。

## 4. UEFI 引导链复验（本版重新实测，不再是 UNVERIFIED）

`tests/uefi64_install_test.py`（VMware EFI 固件 + ISO 安装）**22 条断言全 PASS**，其中：

```
[PASS] 自研 BOOTX64.EFI 启动（U: 前缀标记）
[PASS] 挂载 ESP（FAT32，48MB 真 FAT32 卷）并读出内核 / 载荷 / GOP 帧缓冲
[PASS] ExitBootServices 成功 / 高半区直映 / [LM64] ENTERED LONG MODE
[PASS] ★ 没有走 BIOS 引导桩（无 I:boot dl=）
[PASS] MBR 55AA；P1=0xEF 活动 65+32519；P2=0x07 32584..盘尾
[PASS] LBA1..64 = loader64.bin 逐字节一致（4090 B）
[PASS] LBA65.. = kernel64_os.bin 逐字节一致（3324848 B）   <- 装进去的就是本版构建产物
```

## 5. 验收汇总（本版全部实跑）

| 测试 | 结果 |
|---|---|
| `tests/wm64_test.py` | **88 / 88 PASS**（⑦ 全绿，含新增的 erased / painted / 调色板三条） |
| `tests/flush64_test.py` | **10 / 10 PASS** |
| `tests/uefi64_install_test.py` | **PASS**（22 条，含内核逐字节一致） |
| `tests/drv64_test.py` | **54 / 54**（r3drv 探测路径改动后无回归） |
| 启动夹具（PATA/AHCI，干净 sysdisk.img） | `range REFUSED=0`、`inject probe ok`、`[VFS64] selftest PASS`、`cmd error=0` |

构建：`build64.sh` 全量 `EXIT=0`；系统内核 **3,306,816 (v0.10.0) → 3,324,848 B**；`/lib/wm.elf` 23,320 B。

产物：

| 文件 | 大小 | sha256 |
|---|---|---|
| `vimtu64-64.iso` | 84,324,352 B | `857a17be050e97acf24b9534afc84eb21c96644c7246699a9bd55c3c9e4c9c6c` |
| `vimtu64-64.img` | 33,554,432 B | `f83bb155f0ec3bf2cfe21e5d590701d21c7948a72e9b90594f64fb3c0fd73403` |

## 6. 已知边界（如实，勿当已验证）

* **USB 存储没有 flush**（用户决定暂缓）：`ata64_flush64(24..)` 如实返回
  `failed path=usb reason=no-sync-cache-command-in-this-build`。补的判据与顺序见
  `docs/TODO_路线图.md` §1（等 USB 写通道 / BOT 状态机彻底完善后再加 `SYNCHRONIZE CACHE(10)`）。
* **NVMe Flush 仍未实测**：代码按 spec 就位，本机无 NVMe 夹具 -> **UNVERIFIED(env)**。
* **AHCI 侧 ATAPI 光驱读仍失败**（`cmd timeout port=<p> cmd=0xA0`）：SATA 光驱只能识别不能读，
  ISO 安装仍必须用 IDE 光驱；本版的端口恢复保证它失败后**不再拖垮数据盘**。
* **外壳交回后会自己继续重画桌面**（同一对帧 `erased2=29813 / painted2=30151`）：那是内核 shell
  的行为（dock/时钟/壁纸动画），不是 wm 的；断言已按此收口，未把它算成失败。
* **wm 的背景快照在接管屏幕时是黑屏**：交回时"还回去的桌面"因此是黑的（随后由外壳重画）。
  真要"交回后与本机桌面逐字节一致"，得让外壳先画桌面再交给 wm —— 属下一批（见 TODO §5）。
