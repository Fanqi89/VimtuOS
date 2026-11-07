# 显示器 EDID 与 DDC 通道（v0.9.22 打通读取与上报；v0.9.23 补块 1 / 隔行 / 运行时切模式）

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

### 2.6 ★ v0.9.23：把 EDID 读到 256 B（块 1 = CEA-861 扩展块）

- `boot/loader64.asm` 的 `read_edid`：块 0 读成功后**再发一次**规范内的 `INT10 4F15 BL=01`
  （`CX = 1` 块号，`ES:DI = 0x7680`），**只读固件通道、不碰任何硬件寄存器**；
  只有块 1 首字节是 `0x02`（CEA-861）才把 `BootInfo.edid_size` 记成 **256**，否则如实退回 128。
- `boot/efi/uefi64.c`：EDID 协议的 `SizeOfEdid` 允许拷到 **256 B**，块 1 的 128 字节校验和
  通过才置 `edid_size = 256`，串口行追加 `blocks=<1|2>`。
- `kernel/edid64.cpp`：**对"上层说它好"不信任**，自己再验一遍块 1 的校验和
  （`e64_sum_ok(d + 128)`）；新增对外只读接口 `edid64_raw64()` / `edid64_raw_len64()`。

**实测（本机 QEMU + SeaBIOS vgabios）—— 这条路径拿不到块 1，如实记录：**

| 证据 | 含义 |
| --- | --- |
| loader 诊断 `L:b1=00FF`（`AX=004F`） | 固件**报成功**，但 `0x7680` 处写的是 `00 FF`（块 0 的头）——即无视 `CX`，又把块 0 写了一遍 |
| `[EDID] ext=1 cea=0 vic=0 dtd=0 hdmi=0 skip=0 blocks=1 b1hdr=0000000000000000` | 内核如实报"EDID 声称有 1 个扩展块，但实读只有块 0"；`b1hdr=` 就是 `0x7680` 的首字节 |
| 结论 | **BIOS/VBE 路径在 QEMU/SeaBIOS 上只能拿到块 0** —— 这是固件实现的上限，不是内核不读 |

所以块 1 的**解析**能力由内核自检（合成块）证明，见 2.7；真机上走 UEFI 的
`EFI_EDID_DISCOVERED/ACTIVE`（`SizeOfEdid = 256`）时才可能拿到真块 1；OVMF 不实现该协议。

### 2.7 ★ v0.9.23：CEA-861 扩展块解析（不再是"只记个数"）

块 1 布局：`[0]=0x02`、`[1]=revision`、`[2]=DTD 偏移 d`、`[3]=flags`、
`[4..d-1]` = data block 集合（每块 `tag=(b>>5)&7` / `len=b&0x1F`）、
`d..126` = 扩展块自己的 18 B DTD、`[127]` = 校验和。本批解码三类：

1. **tag 2 = Video Data Block**：每字节是 VIC（bit7 = "native"，先清掉）。VIC 表**只放能逐条核对**的
   `1..16` 与 `31..34`（CEA-861-F Table 3 的标清/高清常用段）；表外的 VIC **如实跳过并计数**
   （`[EDID] ext=… skip=<n>`），绝不编造分辨率。
2. **tag 3 = Vendor Specific**：OUI 小端 `0x000C03` → `hdmi=1`（只做识别，不解析物理尺寸/音频）。
3. **扩展块自己的 18 B DTD**：复用同一个 `e64_parse_dtd()`，进同一张模式表（去重、上限 32 条）。

自检把这三条合成一块假块 1（VIC 4/16 + HDMI OUI + 1920x1080**隔行**扩展 DTD），
串口给可见证据：

```
[EDID64] selftest ext synthetic cea=1 vic=2 dtd=1 hdmi=1 ilace=1
```

### 2.8 ★ v0.9.23：隔行模式不再被丢掉

- `e64_parse_dtd()` 读 DTD flags（`dd[17]`）**bit7 = 隔行**，存进 `Edid64Mode.interlaced` /
  `Edid64.interlaced`。结构体只在模块内部用，**不进 ioctl ABI**。
- 模式表去重键从 `(w,h)` 改成 `(w,h,隔行)` —— 1920x1080p 与 1920x1080i 是**两个**模式。
- 顺手修了建立时序 byte `0x24` 的**表错位**：旧表把 `1024x768@60` 写成 700、`@70` 写成 750；
  现在按 E-EDID 顺序（bit7 起）：800x600@72 / @75 / 832x624@75 / 保留 / **1024x768@87i** /
  1024x768@60 / @70 / @75。
- DRM 上报：`Drm64ModeMode.flags` 对隔行模式置 **`0x10`（`DRM_MODE_FLAG_INTERLACE`）**，
  非隔行仍 0；`[DRM] edid … ilace=<n>` 报条数（与 `[EDID] mode … ilace=1` 必须一致）。

**刷新率口径（必须说清楚）**：EDID 的 `Vtotal` 对隔行模式是**每场行数**，所以算出来的是**场频**
（1024x768@87i 显示成 `87.0Hz`）。要帧频就除以 2，本模块不擅自改口径。

实测原文（QEMU 的内置 EDID 里真的有这一条：建立时序 byte 0x24 bit3）：

```
[EDID] mode i=3 1024x768@87.0Hz kind=1 ilace=1
[DRM]  edid modes=12 cur=1280x800 refresh=75 matched=1 conn_modes=13 mm=320x200 ilace=1
[DRMDEMO] modeswitch list=13 ilace=1 iflags=0x10 pick=800x600 cur=1280x800
```

### 2.9 ★ v0.9.23：运行时真能切分辨率（SETCRTC → Bochs DISPI）

原来 `SETCRTC` 只接受"当前几何"，换分辨率一律 `-EINVAL` —— 也就是**用户态根本没法改模式**。
本批把这条路打通，但只走**真有实现**的那条：

- `d64_modeswitch64(w,h)`：请求的模式必须**在 EDID 模式表里**（逐条核对，不猜）且**非隔行**
  （DISPI 不能隔行扫描），然后调 `fb_set_mode(w,h)`（Bochs VBE DISPI `0x1CE/0x1CF` + **回读校验**）。
  成功后重建 `g_mode64`（`GETCRTC`/`GETCONNECTOR` 的唯一来源）与 evdev 绝对轴区间
  （`ABS_X/Y.maximum = w-1 / h-1`）。
- 做不到就**如实拒绝**：`path=not-in-edid`（模式不在 EDID 表里）或 `path=unsupported`
  （回读与请求不一致：VMware SVGA / virtio-gpu / GOP 都没有"改模式"的命令通道），
  `SETCRTC` 返回 `-EINVAL`。
- ring3 侧（`user/apps/drmdemo.c`）走完整往返：**切过去 → 切回来 → 再上一帧 → 反例**。

实测原文：

```
[DRMDEMO] modeswitch list=13 ilace=1 iflags=0x10 pick=800x600 cur=1280x800
[DRM] setcrtc mode-switch w=800 h=600 ok=1 path=dispi refresh=60 kms=800x600
[DRMDEMO] setcrtc mode=800x600 rc=0
[DRM] setcrtc mode-switch w=1280 h=800 ok=1 path=dispi refresh=75 kms=1280x800
[DRMDEMO] setcrtc restore=1280x800 rc=0
[DRMDEMO] modeswitch reflip rc=0
[DRM] present id=1 path=cpu bytes=…            ← 往返之后 ring3 仍然能上屏
[DRM] setcrtc mode-switch w=1111 h=777 ok=0 path=not-in-edid
[DRMDEMO] setcrtc badmode=1111x777 rc=-22      ← 反例：EDID 表外如实拒绝
```

---

## 3. 验收

### 3.1 `tests/edid64_test.py` —— **31/31**（v0.9.22 是 21/21）

| 判据 | 证据 |
| --- | --- |
| 引导层 EDID 被解析 | `[EDID64] present=1 ver=1.4 mfg=RHT name=QEMU Monitor size=32x20cm` |
| 控制台汇总行 | `[EDID] mfg=… preferred=1280x800@75.0Hz modes=12 src=vbios` |
| 模式列表（首选在 i=0，无 0x0 垃圾，每条带 `ilace=`） | `[EDID] mode i=0 1280x800@75.0Hz kind=0 ilace=0` + 11 条 |
| 显示层顺序修复 | `[DISP64] init modes=16 cur=1280x800@75.0 src=edid` |
| DRM 上报（真实模式数 / 尺寸 / 刷新率 / 隔行条数） | `[DRM] edid modes=12 cur=1280x800 refresh=75 matched=1 conn_modes=13 mm=320x200 ilace=1` |
| **块 1 解析器**（合成块自证） | `[EDID64] selftest ext synthetic cea=1 vic=2 dtd=1 hdmi=1 ilace=1` |
| **块 1 诚实性**：没拿到就必须报 0 | `[EDID] ext=1 cea=0 vic=0 dtd=0 hdmi=0 skip=0 blocks=1 b1hdr=0000000000000000` |
| **隔行模式**被保留并标注 | `[EDID] mode i=3 1024x768@87.0Hz kind=1 ilace=1`，且 `[DRM] edid … ilace=1` 与之一致 |
| UEFI 模式表非空 / EDID 结论如实 | `U:modes=16 max=30`、`U:edid none (firmware has no EDID protocol)` |
| 禁止项 | 无 PANIC / TRIPLE FAULT / `[EDID64] selftest FAIL` |

### 3.2 回归（同一批产物）

| 测试 | 结果 |
| --- | --- |
| `tests/drm64_test.py` | **108/108**（v0.9.22 是 96；新增 12 条 = 隔行 flags + 运行时切分辨率 + 反例 + 切回后上屏） |
| `tests/display64_test.py` | 22/22 |
| `tests/display_runtime_test.py` | 19/19 |
| `tests/sysstate64_test.py` | 101/101（本机串口抓取偶发**行尾截断**，3 次里 2 次红 1 次绿；红的那两次都是"某行少了尾巴"，不是行为回归） |
| `tests/status_report.py` | 95 / 0 / 0 |

---

## 4. 还缺什么（欠账，如实）

1. **运行期 DDC 位翻转**（Intel GMBUS / VGA bit-bang）—— 需要 Intel PRM 的寄存器表才能不猜地写。
2. **块 1 的真数据**：本机 QEMU/SeaBIOS 的 `4F15 BL=01` 无视 `CX` 只回块 0（见 2.6 的证据），
   所以 CEA-861 解析目前只有**合成块**证据；真机 UEFI 固件给出 `SizeOfEdid = 256` 时才会拿到真块 1。
3. **运行时切分辨率只对 Bochs DISPI（QEMU `-vga std`）保证**；VMware SVGA / virtio-gpu / GOP
   如实 `path=unsupported`（这些设备没有改模式的命令通道，写寄存器属于"猜"）。
4. **隔行模式只上报、不扫描**：DISPI 不能隔行输出，`SETCRTC` 对隔行目标一律拒绝。
5. **多显示器**：只有 1 个 connector（HDMI-A-1），EDID 也只读第一条通道。
