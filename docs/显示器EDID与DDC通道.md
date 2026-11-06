# 显示器 EDID 与 DDC 通道（v0.9.22）

> 用户报告："硬件检查报告里显示模块还是 EDID none，系统只能盲猜分辨率"。
> 本篇是这件事的**现状评估 + 本批做了什么 + 还缺什么**。

---

## 1. 现状评估（做之前先说清楚缺什么）

审查结论：**EDID 不是没有，而是只有一半路**，而且有一条**顺序缺陷**让报告永远显示 none。

| 环节 | 做之前的状态 |
| --- | --- |
| 引导层读 EDID（BIOS/VBE 路径） | ✅ 有：`boot/loader64.asm` 的 `read_edid` 调 **INT10 AX=4F15 BL=01**，把 128 B 放在物理 `0x7600`，填 `BootInfo.edid_addr/edid_size/edid_ok` |
| 引导层读 EDID（**UEFI/GOP 路径**） | ❌ **完全没有**：`boot/efi/uefi64.c` 只写了 `edid_addr=0x7600 / edid_size=128`，**从不写 `edid_ok`、也不往 `0x7600` 拷一个字节** |
| 引导层模式表（UEFI） | ❌ `mode_count` 恒为 0（只把"当前 GOP 模式"写进 BootInfo） |
| 内核解析 EDID | ✅ 有：`kernel/edid64.cpp`（厂商 ID / 型号 / 产品码 / 序列号 / 制造年月 / 物理尺寸 / 首选 DTD 的刷新率 / 范围限制） |
| 内核**初始化顺序** | ❌ **真缺陷**：`display64_init64()` 在 `kernel64.cpp` 里比 `edid64_init64()` **早 480 行**跑，于是显示层读到的永远是 `valid=false` → 报告里 `edid=n/a`、`src=none` |
| 模式列表 | ⚠️ 只解析**第一个** DTD；建立时序 / 标准时序一律不解码 → 没有"支持的视频模式列表" |
| DRM/KMS 上报 | ❌ `GETCONNECTOR` 只回 1 个模式，`mm_width/mm_height` 恒 0，`vrefresh` 是**固定 60 的估计值** |
| 运行期 DDC/I2C（GMBUS / bit-bang） | ❌ **没有**（`display64` 里如实打 `ddc runtime re-probe skipped (i2c bit-bang risk; boot-time EDID only)`） |
| EDID 扩展块（CEA-861） | ❌ 不解码（只记 `ext_count`） |

一句话：**用户看到的 "EDID none" 有 90% 是"初始化顺序"造成的；剩下的部分在真机（UEFI 引导）上才是真的没读。**

---

## 2. 本批做了什么

### 2.1 修掉顺序缺陷（真缺陷）

`kernel/kernel64.cpp`：`edid64_init64()` 提到 `display64_init64()` **之前**
（它只读 BootInfo + 恒等映射里的 `0x7600`，不碰 fb、不写硬件，提前调用是安全的）。

效果：`[DISP64] init modes=16 cur=1280x800@75.0 **src=edid**`，设置页/硬件报告里也变成
`[UI] settings display hz=75.0Hz source=edid preferred-timing`（原来是 `none` / `n/a`）。

### 2.2 UEFI 路径真读 EDID（真机走的就是这条）

`boot/efi/uefi64.c` 新增两件**规范内**的事（不猜任何寄存器）：

1. **GOP 模式表**：`QueryMode(0..MaxMode-1)` 枚举全部 32bpp 线性模式 → 按 `BootMode` 格式
   （8 B/项，与 BIOS 路径同格式）写进 `BootInfo.mode_list`（`0x7400`），`mode_count` 不再是 0。
2. **EDID 协议**：
   * 先 `LocateProtocol(EFI_EDID_DISCOVERED_PROTOCOL_GUID = 1C0C34F6-D380-41FA-A049-8AD06C1A66AA)`；
   * 拿不到再 `EFI_EDID_ACTIVE_PROTOCOL_GUID = BDED4436-756C-4E21-8011-4755002FA5AF`；
   * 校验头部 `00 FF FF FF FF FF FF 00` + 128 字节校验和（sum & 0xFF == 0）→ 通过才置 `edid_ok=1`；
   * 若首选分辨率在枚举出的模式表里且与当前不同 → `SetMode()` 切过去，并**刷新 BootInfo 的几何/LFB**
     （真机上 GOP 帧缓冲可能 > 4GiB，所以高 32 位一起更新）。

实测日志（`-drive if=pflash` 用 OVMF）：
```
U:modes=16 max=30
U:edid none (firmware has no EDID protocol)
```
—— QEMU 的 OVMF **没有**实现这两个 EDID 协议，所以本机只能验证到"枚举模式表 + 如实回答 none"；
真机（笔记本/主板 UEFI）绝大多数都会实现 `EDID_DISCOVERED`，届时会走 `U:edid ok=1 …` + `U:setmode …`。
**这一点如实写在测试与发布说明里，不声称"UEFI 的 EDID 已验证"。**

### 2.3 模式列表补齐

`kernel/edid64.cpp` 新增 `e64_parse_modes()`：

* **4 个 DTD**（首选在 `i=0`）—— 完整解析 w/h/pclk/刷新率；
* **建立时序**：byte `0x23` / `0x24` 逐位展开（`0x25` 只有 bit7 = 1152x870@75；`1024x768@87i` 是隔行，跳过）；
* **标准时序**：`0x26..0x35` 8 项，`h = (b1+31)*8`，宽高比 `0b00=16:10 / 01=4:3 / 10=5:4 / 11=16:9`，
  刷新 = `(b2 & 0x3F) + 60`；
* 24 条上限、按 (w,h) 去重。

控制台汇总行（用户要求的三件事：厂商 + 最佳分辨率 + 刷新率）：

```
[EDID] mfg=RHT name=QEMU Monitor product=4660 serial=0 year=2014 cm=32x20 preferred=1280x800@75.0Hz modes=12 src=vbios
[EDID] mode i=0 1280x800@75.0Hz kind=0
[EDID] mode i=1 640x480@60.0Hz kind=1
...
```

### 2.4 接到 DRM/KMS

`kernel/drm64.cpp`：

* `GETCONNECTOR` 上报**真实模式列表**（`[0]` = 当前模式，其后是 EDID 的 N 条）+ 物理尺寸
  （`mm = size_cm × 10`），两阶段语义照 Linux（`count_modes` 不够就只回个数）；
* 当前模式的 `vrefresh` 取 EDID 里同分辨率的**真实刷新率**（不再固定 60）。

实测：
```
[DRM] edid modes=12 cur=1280x800 refresh=75 matched=1 conn_modes=13 mm=320x200
```

### 2.5 为什么**没有**顺手写 I2C/DDC 位翻转（GMBUS / 0x3C4 bit-bang）

用户建议"实现最基础的 I2C 读写"。这里故意**没做**，理由写在明处：

1. **Intel GMBUS 的寄存器位布局**（`GMBUS0/1/2/3` 的 pin select、cycle、byte count、HW-ready/NAK 位）
   本机**没有可核对的一手参考**（没有 `i915` 头文件，也不联网）。按本项目的规矩"命令号/寄存器位不猜"，
   写错的位可能把 GMBUS 引擎挂死（显示本身还在 LFB 上，但那个通道就废了）。
2. **VGA 端口位翻转（bit-bang）** 的经典实现依赖具体芯片组的 GPIO 映射，同样属于"没有参考就不能写"。
3. 更重要的是：**在用户的场景（UEFI 真机）上，规范内的 EDID 协议已经把这件事解决了**；
   GMBUS 的价值是在"固件不给 EDID"的机器上兜底 —— 那需要 Intel PRM 的寄存器表，属于单独一批。

**所以本批的边界是**：EDID 的读取走"固件通道"（BIOS/VBE + UEFI EDID 协议），解析与上报全部做完；
运行期 DDC 位翻转留作欠账（`display64` 那行 `ddc runtime re-probe skipped` 仍然如实存在）。

---

## 3. 验收（`tests/edid64_test.py`，21/21）

| 判据 | 证据 |
| --- | --- |
| 引导层 EDID 被解析 | `[EDID64] present=1 ver=1.4 mfg=RHT name=QEMU Monitor size=32x20cm` |
| 控制台汇总行 | `[EDID] mfg=… product=4660 … cm=32x20 preferred=1280x800@75.0Hz modes=12 src=vbios` |
| 模式列表（首选在 i=0，无 0x0 垃圾） | `[EDID] mode i=0 1280x800@75.0Hz kind=0` + 12 条 |
| 显示层顺序修复 | `[DISP64] init modes=16 cur=1280x800@75.0 src=edid` |
| DRM 上报真实模式数/尺寸/刷新率 | `[DRM] edid modes=12 cur=1280x800 refresh=75 matched=1 conn_modes=13 mm=320x200` |
| UEFI 模式表非空 | `U:modes=16 max=30` |
| UEFI EDID 结论如实 | `U:edid none (firmware has no EDID protocol)`（OVMF 未实现该协议） |
| 禁止项 | 无 PANIC / TRIPLE FAULT / `[EDID64] selftest FAIL` |

回归：`drm64_test` **96/96**、`display64_test` **22/22**、`display_runtime_test` **19/19**、
`sysstate64_test` **101/101**、`status_report` **95/0/0**。

---

## 4. 还缺什么（欠账，如实）

1. **运行期 DDC 位翻转**（Intel GMBUS / VGA bit-bang）—— 需要 Intel PRM 的寄存器表才能不猜地写。
2. **EDID 扩展块（CEA-861）**：`ext_count > 0` 时仍只记数字，扩展块里的 HDMI 详细时序/音频不解析。
3. **隔行模式**（如 1024x768@87i）不报。
4. **运行时切分辨率**：`U:setmode` 是**引导期**在 UEFI 里做的；内核侧仍只"报告"，不主动改模式
   （改模式涉及各家 GPU 的显示管线，不在本批）。
5. **多显示器**：只有 1 个 connector（HDMI-A-1），EDID 也只读第一条通道。
